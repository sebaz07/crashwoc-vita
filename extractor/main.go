// crashwoc-extract: copies the game files from your own Crash Bandicoot: The
// Wrath of Cortex (GameCube, USA, GCBE7D) disc image into the folder layout
// the PS Vita port reads (ux0:data/crashwoc/files/...).
//
// Supported images: .iso / .gcm (plain), .nkit.iso (GameCube NKit), .ciso.
// RVZ / WIA / GCZ: convert to ISO first with Dolphin (right click > Convert File).
//
// Usage: crashwoc-extract [disc image] [output folder]
//
//	crashwoc-extract --dol main.dol <disc image>   (developers: the DOL for building)
//
// On Windows, without arguments (or with an image dragged onto the .exe) it
// opens a small window: pick the image, then the Vita (VitaShell USB mode) or
// a folder, and press Extract. Elsewhere it asks in the terminal.
package main

import (
	"bufio"
	"bytes"
	"crypto/sha1"
	"crypto/sha256"
	_ "embed"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

const version = "1.2.0"

// file list of the USA disc (path, size, SHA-256): used to check the result
//
//go:embed manifest.csv
var manifestCSV string

type manifestEntry struct {
	size int64
	sha  string
}

func loadManifest() map[string]manifestEntry {
	m := map[string]manifestEntry{}
	for _, line := range strings.Split(manifestCSV, "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		f := strings.Split(line, ",")
		if len(f) != 3 {
			continue
		}
		n, _ := strconv.ParseInt(f[1], 10, 64)
		m[strings.ToLower(f[0])] = manifestEntry{n, f[2]}
	}
	return m
}

// ---------------------------------------------------------------- images

// disc is a random-access view of the decoded disc
type disc interface {
	io.ReaderAt
	Size() int64
}

type plainDisc struct {
	f    *os.File
	size int64
}

func (d *plainDisc) ReadAt(p []byte, off int64) (int, error) { return d.f.ReadAt(p, off) }
func (d *plainDisc) Size() int64                             { return d.size }

// CISO (Nintendont / USB Loader): 0x8000-byte header with a block map
type cisoDisc struct {
	f      *os.File
	bs     int64
	index  []int64 // block -> data block number, -1 when absent (zeros)
	blocks int64
}

func openCISO(f *os.File) (*cisoDisc, error) {
	hdr := make([]byte, 0x8000)
	if _, err := f.ReadAt(hdr, 0); err != nil {
		return nil, err
	}
	bs := int64(binary.LittleEndian.Uint32(hdr[4:8]))
	if bs == 0 || bs&(bs-1) != 0 {
		return nil, errors.New("CISO: invalid block size")
	}
	d := &cisoDisc{f: f, bs: bs}
	var n int64
	for i := 8; i < 0x8000; i++ {
		if hdr[i] != 0 {
			d.index = append(d.index, n)
			n++
		} else {
			d.index = append(d.index, -1)
		}
	}
	d.blocks = int64(len(d.index))
	return d, nil
}

func (d *cisoDisc) Size() int64 { return d.blocks * d.bs }

func (d *cisoDisc) ReadAt(p []byte, off int64) (int, error) {
	done := 0
	for done < len(p) {
		blk := (off + int64(done)) / d.bs
		in := (off + int64(done)) % d.bs
		n := int(d.bs - in)
		if n > len(p)-done {
			n = len(p) - done
		}
		if blk >= d.blocks {
			return done, io.EOF
		}
		if d.index[blk] < 0 {
			for i := 0; i < n; i++ {
				p[done+i] = 0
			}
		} else {
			if _, err := d.f.ReadAt(p[done:done+n], 0x8000+d.index[blk]*d.bs+in); err != nil {
				return done, err
			}
		}
		done += n
	}
	return done, nil
}

func openDisc(path string) (disc, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	st, err := f.Stat()
	if err != nil {
		return nil, err
	}
	magic := make([]byte, 4)
	if _, err := f.ReadAt(magic, 0); err != nil {
		return nil, err
	}
	switch {
	case bytes.Equal(magic, []byte("CISO")):
		return openCISO(f)
	case bytes.Equal(magic, []byte("RVZ\x01")), bytes.Equal(magic, []byte("WIA\x01")),
		binary.LittleEndian.Uint32(magic) == 0xB10BC001:
		return nil, errors.New("this is a compressed Dolphin image (RVZ/WIA/GCZ). Convert it to ISO in Dolphin: right click the game > Convert File... > Format: ISO.\n" +
			"   Es una imagen comprimida de Dolphin (RVZ/WIA/GCZ). Conviertela a ISO en Dolphin: clic derecho > Convertir archivo... > Formato: ISO.")
	}
	return &plainDisc{f, st.Size()}, nil
}

// ---------------------------------------------------------------- GameCube file system

type fstFile struct {
	path   string
	offset int64
	size   int64
}

func be32(b []byte) uint32 { return binary.BigEndian.Uint32(b) }

func readFST(d disc) (id string, rev byte, files []fstFile, err error) {
	hdr := make([]byte, 0x440)
	if _, err = d.ReadAt(hdr, 0); err != nil {
		return
	}
	if be32(hdr[0x1C:]) != 0xC2339F3D {
		err = errors.New("not a GameCube disc image / no es una imagen de disco de GameCube")
		return
	}
	id = string(hdr[0:6])
	rev = hdr[7]
	fstOff := int64(be32(hdr[0x424:]))
	fstSize := int64(be32(hdr[0x428:]))
	if fstSize < 12 || fstSize > 64<<20 || fstOff+fstSize > d.Size() {
		err = errors.New("damaged file table (FST) / tabla de archivos danada")
		return
	}
	fst := make([]byte, fstSize)
	if _, err = d.ReadAt(fst, fstOff); err != nil {
		return
	}
	n := int(be32(fst[8:]))
	if n <= 0 || n*12 > len(fst) {
		err = errors.New("damaged file table (FST) / tabla de archivos danada")
		return
	}
	names := fst[n*12:]
	name := func(i int) string {
		o := int(be32(fst[i*12:]) & 0xFFFFFF)
		if o >= len(names) {
			return ""
		}
		e := bytes.IndexByte(names[o:], 0)
		if e < 0 {
			e = len(names) - o
		}
		return string(names[o : o+e])
	}
	// walk: directory entries hold the index one past their last child
	type dir struct {
		path string
		end  int
	}
	stack := []dir{{"", n}}
	for i := 1; i < n; i++ {
		for len(stack) > 1 && i >= stack[len(stack)-1].end {
			stack = stack[:len(stack)-1]
		}
		e := fst[i*12 : i*12+12]
		nm := name(i)
		if nm == "" || nm == "." || nm == ".." || strings.ContainsAny(nm, "/\\:") {
			err = fmt.Errorf("bad file name in FST: %q", nm)
			return
		}
		p := nm
		if stack[len(stack)-1].path != "" {
			p = stack[len(stack)-1].path + "/" + nm
		}
		if e[0]&1 != 0 {
			stack = append(stack, dir{p, int(be32(e[8:]))})
			continue
		}
		f := fstFile{p, int64(be32(e[4:])), int64(be32(e[8:]))}
		if f.offset+f.size > d.Size() {
			err = fmt.Errorf("file %s lies past the end of the image: incomplete dump? / el archivo queda fuera de la imagen: volcado incompleto?", p)
			return
		}
		files = append(files, f)
	}
	return
}

// ---------------------------------------------------------------- main

func waitEnter(interactive bool) {
	if interactive {
		fmt.Print("\nPress Enter to close / Pulsa Enter para cerrar...")
		bufio.NewReader(os.Stdin).ReadString('\n')
	}
}

func fail(interactive bool, format string, a ...interface{}) {
	fmt.Printf("\nERROR: "+format+"\n", a...)
	waitEnter(interactive)
	os.Exit(1)
}

// writeDOL saves sys/main.dol (for building the port from source)
func writeDOL(d disc, dst string) error {
	hdr := make([]byte, 0x440)
	if _, err := d.ReadAt(hdr, 0); err != nil {
		return err
	}
	off := int64(be32(hdr[0x420:]))
	dh := make([]byte, 0x100)
	if _, err := d.ReadAt(dh, off); err != nil {
		return err
	}
	var size int64 = 0x100
	for i := 0; i < 18; i++ {
		if e := int64(be32(dh[i*4:])) + int64(be32(dh[0x90+i*4:])); e > size {
			size = e
		}
	}
	buf := make([]byte, size)
	if _, err := d.ReadAt(buf, off); err != nil {
		return err
	}
	sum := sha1hex(buf)
	if sum != "c9cbd49a9eb0006f55533eb7d0fb5ebe2a73b72f" {
		fmt.Printf("WARNING: main.dol SHA-1 %s is not the expected GCBE7D one\n", sum)
	}
	return os.WriteFile(dst, buf, 0o644)
}

type extractResult struct {
	id      string
	rev     byte
	files   int
	bad     int
	missing int
	dest    string // .../crashwoc
}

// openAndCheck opens the image and checks it is the USA disc
func openAndCheck(img string) (disc, string, byte, []fstFile, error) {
	d, err := openDisc(img)
	if err != nil {
		return nil, "", 0, nil, err
	}
	id, rev, files, err := readFST(d)
	if err != nil {
		return nil, "", 0, nil, err
	}
	if id != "GCBE7D" {
		return nil, id, rev, nil, fmt.Errorf("this port needs the USA version (GCBE7D), this disc is %s.\nEste port necesita la version USA (GCBE7D); este disco es %s.", id, id)
	}
	return d, id, rev, files, nil
}

// extractGame copies every file of the disc to <out>/crashwoc/files and checks
// it against the manifest; progress gets (bytes done, bytes total).
func extractGame(img, out string, progress func(done, total int64)) (*extractResult, error) {
	d, id, rev, files, err := openAndCheck(img)
	if err != nil {
		return nil, err
	}
	man := loadManifest()
	root := filepath.Join(out, "crashwoc", "files")
	if err := os.MkdirAll(root, 0o755); err != nil {
		return nil, err
	}
	var total, done int64
	for _, f := range files {
		total += f.size
	}
	buf := make([]byte, 1<<20)
	res := &extractResult{id: id, rev: rev, files: len(files), dest: filepath.Join(out, "crashwoc")}
	seen := map[string]bool{}
	for _, f := range files {
		dst := filepath.Join(root, filepath.FromSlash(f.path))
		if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
			return nil, err
		}
		w, err := os.Create(dst)
		if err != nil {
			return nil, err
		}
		h := sha256.New()
		for o := int64(0); o < f.size; {
			n := int64(len(buf))
			if n > f.size-o {
				n = f.size - o
			}
			if _, err := d.ReadAt(buf[:n], f.offset+o); err != nil && err != io.EOF {
				w.Close()
				return nil, fmt.Errorf("reading %s: %v", f.path, err)
			}
			if _, err := w.Write(buf[:n]); err != nil {
				w.Close()
				return nil, fmt.Errorf("writing %s: %v", dst, err)
			}
			h.Write(buf[:n])
			o += n
			done += n
			if progress != nil {
				progress(done, total)
			}
		}
		if err := w.Close(); err != nil {
			return nil, fmt.Errorf("writing %s: %v", dst, err)
		}
		key := strings.ToLower(f.path)
		seen[key] = true
		if m, ok := man[key]; ok && (m.size != f.size || m.sha != hex.EncodeToString(h.Sum(nil))) {
			res.bad++
		}
	}
	for k := range man {
		if !seen[k] {
			res.missing++
		}
	}
	return res, nil
}

// gameDataSize: bytes the extracted files take (from the manifest)
func gameDataSize() int64 {
	var n int64
	for _, m := range loadManifest() {
		n += m.size
	}
	return n
}

func main() {
	args := os.Args[1:]
	// Windows: a small window unless real command-line arguments are given
	if guiMain(args) {
		return
	}
	interactive := len(args) <= 1
	if len(args) > 0 && (args[0] == "-h" || args[0] == "--help") {
		fmt.Println("usage: crashwoc-extract [disc image] [output folder]")
		fmt.Println("       crashwoc-extract --dol main.dol <disc image>")
		return
	}
	if len(args) == 3 && args[0] == "--dol" {
		d, err := openDisc(args[2])
		if err != nil {
			fail(false, "%v", err)
		}
		if err := writeDOL(d, args[1]); err != nil {
			fail(false, "%v", err)
		}
		fmt.Println("wrote", args[1])
		return
	}
	fmt.Printf("Crash Bandicoot: The Wrath of Cortex - PS Vita data extractor %s\n", version)
	fmt.Println("Uses YOUR OWN GameCube disc (USA, GCBE7D) / Usa TU PROPIO disco de GameCube (USA, GCBE7D)")
	fmt.Println()
	exeDir := "."
	if exe, err := os.Executable(); err == nil {
		exeDir = filepath.Dir(exe)
	}
	img := ""
	if len(args) >= 1 {
		img = args[0]
	} else {
		fmt.Print("Drag the disc image (.iso / .nkit.iso / .ciso) here and press Enter\nArrastra aqui la imagen del disco y pulsa Enter:\n> ")
		line, _ := bufio.NewReader(os.Stdin).ReadString('\n')
		img = strings.Trim(strings.TrimSpace(line), "\"'")
	}
	if img == "" {
		fail(interactive, "no image given / no se indico ninguna imagen")
	}
	out := filepath.Join(exeDir, "crashwoc-vita-data")
	if len(args) >= 2 {
		out = args[1]
	}
	lastPct := -1
	res, err := extractGame(img, out, func(done, total int64) {
		if pct := int(done * 100 / max64(total, 1)); pct != lastPct {
			lastPct = pct
			fmt.Printf("\rExtracting / Extrayendo... %3d%%", pct)
		}
	})
	if err != nil {
		fail(interactive, "%v", err)
	}
	fmt.Printf("\rExtracting / Extrayendo... 100%%\n")
	fmt.Printf("Disc / Disco: %s (revision %d), %d files\n\n", res.id, res.rev, res.files)
	if res.bad > 0 || res.missing > 0 {
		fmt.Printf("WARNING: %d files differ and %d are missing compared to a good USA dump.\n", res.bad, res.missing)
		fmt.Printf("AVISO: %d archivos no coinciden y faltan %d respecto a un volcado USA correcto.\n", res.bad, res.missing)
		fmt.Println("The game may crash. Check your dump. / El juego puede fallar. Revisa tu volcado.")
	} else {
		fmt.Println("All files verified OK / Todos los archivos verificados OK")
	}
	fmt.Printf("\nDone / Listo: %s\n\n", res.dest)
	fmt.Println("Copy the \"crashwoc\" folder to ux0:data/ on your Vita (ux0:data/crashwoc/files/...).")
	fmt.Println("Copia la carpeta \"crashwoc\" a ux0:data/ en tu Vita (ux0:data/crashwoc/files/...).")
	waitEnter(interactive)
}

// looksLikeVita: the root of ux0: has app/ and data/
func looksLikeVita(dir string) bool {
	for _, d := range []string{"app", "data"} {
		if st, err := os.Stat(filepath.Join(dir, d)); err != nil || !st.IsDir() {
			return false
		}
	}
	return true
}

func sha1hex(b []byte) string {
	h := sha1.Sum(b)
	return hex.EncodeToString(h[:])
}

func max64(a, b int64) int64 {
	if a > b {
		return a
	}
	return b
}
