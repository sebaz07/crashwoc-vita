//go:build windows

// Small native window (plain Win32, no cgo): choose the disc image, choose the
// Vita (VitaShell USB mode) or a folder, press Extract.
package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"unsafe"
)

var (
	gdi32    = syscall.NewLazyDLL("gdi32.dll")
	comctl32 = syscall.NewLazyDLL("comctl32.dll")

	procRegisterClassExW    = user32.NewProc("RegisterClassExW")
	procCreateWindowExW     = user32.NewProc("CreateWindowExW")
	procDefWindowProcW      = user32.NewProc("DefWindowProcW")
	procGetMessageW         = user32.NewProc("GetMessageW")
	procIsDialogMessageW    = user32.NewProc("IsDialogMessageW")
	procTranslateMessage    = user32.NewProc("TranslateMessage")
	procDispatchMessageW    = user32.NewProc("DispatchMessageW")
	procPostQuitMessage     = user32.NewProc("PostQuitMessage")
	procPostMessageW        = user32.NewProc("PostMessageW")
	procSendMessageW        = user32.NewProc("SendMessageW")
	procShowWindow          = user32.NewProc("ShowWindow")
	procUpdateWindow        = user32.NewProc("UpdateWindow")
	procEnableWindow        = user32.NewProc("EnableWindow")
	procSetWindowTextW      = user32.NewProc("SetWindowTextW")
	procGetWindowTextW      = user32.NewProc("GetWindowTextW")
	procGetWindowTextLength = user32.NewProc("GetWindowTextLengthW")
	procLoadCursorW         = user32.NewProc("LoadCursorW")
	procLoadIconW           = user32.NewProc("LoadIconW")
	procGetSysColorBrush    = user32.NewProc("GetSysColorBrush")
	procAdjustWindowRectEx  = user32.NewProc("AdjustWindowRectEx")
	procGetSystemMetrics    = user32.NewProc("GetSystemMetrics")
	procSetProcessDPIAware  = user32.NewProc("SetProcessDPIAware")
	procGetDC               = user32.NewProc("GetDC")
	procReleaseDC           = user32.NewProc("ReleaseDC")
	procDestroyWindow       = user32.NewProc("DestroyWindow")
	procGetDeviceCaps       = gdi32.NewProc("GetDeviceCaps")
	procCreateFontW         = gdi32.NewProc("CreateFontW")
	procInitCommonControls  = comctl32.NewProc("InitCommonControlsEx")
	procGetModuleHandleW    = kernel32.NewProc("GetModuleHandleW")
	procAttachConsole       = kernel32.NewProc("AttachConsole")
	procGetStdHandle        = kernel32.NewProc("GetStdHandle")
	procGetUserDefaultUILng = kernel32.NewProc("GetUserDefaultUILanguage")
	procGetLogicalDrives    = kernel32.NewProc("GetLogicalDrives")
	procGetDiskFreeSpaceExW = kernel32.NewProc("GetDiskFreeSpaceExW")
)

const (
	wsOverlapped  = 0x00000000
	wsCaption     = 0x00C00000
	wsSysMenu     = 0x00080000
	wsMinimizeBox = 0x00020000
	wsVisible     = 0x10000000
	wsChild       = 0x40000000
	wsTabStop     = 0x00010000
	wsGroup       = 0x00020000
	wsBorder      = 0x00800000
	wsExClientEdg = 0x00000200
	esAutoHScroll = 0x0080
	bsPush        = 0x0000
	bsDefPush     = 0x0001
	bsAutoRadio   = 0x0009
	ssLeft        = 0x0000

	wmDestroy  = 0x0002
	wmClose    = 0x0010
	wmSetFont  = 0x0030
	wmCommand  = 0x0111
	wmApp      = 0x8000
	bmSetCheck = 0x00F1
	bmGetCheck = 0x00F0
	pbmSetRng  = 0x0406
	pbmSetPos  = 0x0402
	bnClicked  = 0

	idImgEdit = 101
	idImgBtn  = 102
	idVita    = 103
	idPC      = 104
	idDstEdit = 105
	idDstBtn  = 106
	idGo      = 107
	idQuit    = 108

	msgProgress = wmApp + 1
	msgDone     = wmApp + 2
)

type wndClassExW struct {
	cbSize        uint32
	style         uint32
	lpfnWndProc   uintptr
	cbClsExtra    int32
	cbWndExtra    int32
	hInstance     uintptr
	hIcon         uintptr
	hCursor       uintptr
	hbrBackground uintptr
	lpszMenuName  *uint16
	lpszClassName *uint16
	hIconSm       uintptr
}

type msgT struct {
	hwnd    uintptr
	message uint32
	wParam  uintptr
	lParam  uintptr
	time    uint32
	pt      [2]int32
	private uint32
}

type rectT struct{ left, top, right, bottom int32 }

type initCommonControlsExT struct {
	size, icc uint32
}

var gui struct {
	hwnd                        uintptr
	imgEdit, dstEdit, vita, pc  uintptr
	imgBtn, dstBtn, goBtn, quit uintptr
	progress, status            uintptr
	font, bold                  uintptr
	dpi                         int
	es                          bool
	busy                        bool
	pcDir                       string
	mu                          sync.Mutex
	statusText                  string
	result                      *extractResult
	err                         error
	toVita                      bool
}

func tr(en, es string) string {
	if gui.es {
		return es
	}
	return en
}

func px(v int) uintptr { return uintptr(v * gui.dpi / 96) }

// guiMain shows the window when started without arguments (or with an image
// dragged onto the .exe). With real arguments it attaches to the parent
// console and returns false so the command-line path runs.
func guiMain(args []string) bool {
	if len(args) >= 2 || (len(args) == 1 && strings.HasPrefix(args[0], "-")) {
		attachParentConsole()
		return false
	}
	runGUI(args)
	return true
}

func attachParentConsole() {
	if r, _, _ := procAttachConsole.Call(^uintptr(0)); r == 0 { // ATTACH_PARENT_PROCESS
		return
	}
	if h, _, _ := procGetStdHandle.Call(uintptr(0xFFFFFFF5)); h != 0 && h != ^uintptr(0) { // STD_OUTPUT_HANDLE
		os.Stdout = os.NewFile(h, "stdout")
	}
	if h, _, _ := procGetStdHandle.Call(uintptr(0xFFFFFFF4)); h != 0 && h != ^uintptr(0) { // STD_ERROR_HANDLE
		os.Stderr = os.NewFile(h, "stderr")
	}
	fmt.Println()
}

func setText(h uintptr, s string) {
	procSetWindowTextW.Call(h, uintptr(unsafe.Pointer(utf16z(s))))
}

func getText(h uintptr) string {
	n, _, _ := procGetWindowTextLength.Call(h)
	buf := make([]uint16, n+1)
	procGetWindowTextW.Call(h, uintptr(unsafe.Pointer(&buf[0])), n+1)
	return syscall.UTF16ToString(buf)
}

func enable(h uintptr, on bool) {
	v := uintptr(0)
	if on {
		v = 1
	}
	procEnableWindow.Call(h, v)
}

func child(class, text string, style uintptr, x, y, w, h int, id uintptr) uintptr {
	hw, _, _ := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(utf16z(class))), uintptr(unsafe.Pointer(utf16z(text))),
		wsChild|wsVisible|style, px(x), px(y), px(w), px(h), gui.hwnd, id, 0, 0)
	procSendMessageW.Call(hw, wmSetFont, gui.font, 1)
	return hw
}

func editBox(x, y, w, h int, id uintptr) uintptr {
	hw, _, _ := procCreateWindowExW.Call(wsExClientEdg, uintptr(unsafe.Pointer(utf16z("EDIT"))), 0,
		wsChild|wsVisible|wsTabStop|esAutoHScroll, px(x), px(y), px(w), px(h), gui.hwnd, id, 0, 0)
	procSendMessageW.Call(hw, wmSetFont, gui.font, 1)
	return hw
}

// findVitaDrive: the first drive whose root has app\ and data\ (ux0: shown by
// VitaShell's USB mode)
func findVitaDrive() string {
	mask, _, _ := procGetLogicalDrives.Call()
	for i := 2; i < 26; i++ { // skip A: B:
		if mask&(1<<uint(i)) == 0 {
			continue
		}
		root := fmt.Sprintf("%c:\\", 'A'+i)
		if looksLikeVita(root) {
			return root
		}
	}
	return ""
}

func freeBytes(dir string) int64 {
	var free uint64
	r, _, _ := procGetDiskFreeSpaceExW.Call(uintptr(unsafe.Pointer(utf16z(dir))), uintptr(unsafe.Pointer(&free)), 0, 0)
	if r == 0 {
		return -1
	}
	return int64(free)
}

func checked(h uintptr) bool {
	r, _, _ := procSendMessageW.Call(h, bmGetCheck, 0, 0)
	return r == 1
}

func refreshDest() {
	if checked(gui.vita) {
		if d := findVitaDrive(); d != "" {
			setText(gui.dstEdit, d)
			setStatus(tr("PS Vita found at "+d+" — press Extract.", "PS Vita encontrada en "+d+": pulsa Extraer."))
		} else {
			setText(gui.dstEdit, "")
			setStatus(tr("Connect the Vita: in VitaShell press START > USB connection, plug the cable, then press Browse… and pick its drive.",
				"Conecta la Vita: en VitaShell pulsa START > Conexión USB, conecta el cable y pulsa Examinar… para elegir su unidad."))
		}
	} else {
		setText(gui.dstEdit, gui.pcDir)
		setStatus(tr("The files will be saved in this folder; then copy the \"crashwoc\" folder to ux0:data/ on the Vita.",
			"Los archivos se guardarán en esta carpeta; después copia la carpeta \"crashwoc\" a ux0:data/ en la Vita."))
	}
}

func setStatus(s string) { setText(gui.status, s) }

func setBusy(b bool) {
	gui.busy = b
	for _, h := range []uintptr{gui.imgEdit, gui.imgBtn, gui.vita, gui.pc, gui.dstEdit, gui.dstBtn, gui.goBtn} {
		enable(h, !b)
	}
}

func startExtract() {
	img := strings.Trim(strings.TrimSpace(getText(gui.imgEdit)), "\"")
	dst := strings.Trim(strings.TrimSpace(getText(gui.dstEdit)), "\"")
	if img == "" {
		showInfo("Crash WOC", tr("Choose your disc image first (step 1).", "Primero elige tu imagen del disco (paso 1)."))
		return
	}
	if dst == "" {
		showInfo("Crash WOC", tr("Choose where to put the files (step 2).", "Elige dónde poner los archivos (paso 2)."))
		return
	}
	gui.toVita = checked(gui.vita)
	out := dst
	if gui.toVita {
		if !looksLikeVita(dst) {
			if askYesNoCancel("Crash WOC", tr("This does not look like a Vita memory card (no app and data folders). Write there anyway?",
				"Esto no parece la memoria de una Vita (no tiene las carpetas app y data). ¿Escribir ahí de todos modos?")) != 1 {
				return
			}
		}
		out = filepath.Join(dst, "data")
	}
	if free := freeBytes(dst); free >= 0 && free < gameDataSize()+(16<<20) {
		showInfo("Crash WOC", tr(fmt.Sprintf("Not enough free space: about %d MB are needed.", gameDataSize()>>20),
			fmt.Sprintf("No hay espacio suficiente: se necesitan unos %d MB.", gameDataSize()>>20)))
		return
	}
	setBusy(true)
	procSendMessageW.Call(gui.progress, pbmSetPos, 0, 0)
	setStatus(tr("Checking the disc…", "Revisando el disco…"))
	go func() {
		lastPct := -1
		res, err := extractGame(img, out, func(done, total int64) {
			if pct := int(done * 100 / max64(total, 1)); pct != lastPct {
				lastPct = pct
				procPostMessageW.Call(gui.hwnd, msgProgress, uintptr(pct), 0)
			}
		})
		gui.mu.Lock()
		gui.result, gui.err = res, err
		gui.mu.Unlock()
		procPostMessageW.Call(gui.hwnd, msgDone, 0, 0)
	}()
}

func finish() {
	gui.mu.Lock()
	res, err := gui.result, gui.err
	gui.mu.Unlock()
	setBusy(false)
	if err != nil {
		procSendMessageW.Call(gui.progress, pbmSetPos, 0, 0)
		setStatus(tr("Error: ", "Error: ") + err.Error())
		showInfo("Crash WOC", err.Error())
		return
	}
	procSendMessageW.Call(gui.progress, pbmSetPos, 100, 0)
	var msg string
	if res.bad > 0 || res.missing > 0 {
		msg = tr(fmt.Sprintf("Done, but %d files differ and %d are missing compared to a good USA dump. The game may crash: check your dump.", res.bad, res.missing),
			fmt.Sprintf("Terminado, pero %d archivos no coinciden y faltan %d respecto a un volcado USA correcto. El juego puede fallar: revisa tu volcado.", res.bad, res.missing))
	} else if gui.toVita {
		msg = tr("Done! All "+fmt.Sprint(res.files)+" files verified and copied to the Vita (ux0:data/crashwoc).\nClose the USB connection in VitaShell and install crashwoc.vpk.",
			"¡Listo! Los "+fmt.Sprint(res.files)+" archivos están verificados y copiados en la Vita (ux0:data/crashwoc).\nCierra la conexión USB en VitaShell e instala crashwoc.vpk.")
	} else {
		msg = tr("Done! All "+fmt.Sprint(res.files)+" files verified.\nCopy the folder "+res.dest+" to ux0:data/ on the Vita, then install crashwoc.vpk.",
			"¡Listo! Los "+fmt.Sprint(res.files)+" archivos están verificados.\nCopia la carpeta "+res.dest+" a ux0:data/ en la Vita e instala crashwoc.vpk.")
	}
	setStatus(msg)
	showInfo("Crash WOC", msg)
}

func wndProc(hwnd, msg, wParam, lParam uintptr) uintptr {
	switch msg {
	case wmCommand:
		id, code := wParam&0xFFFF, (wParam>>16)&0xFFFF
		if code != bnClicked {
			break
		}
		switch id {
		case idImgBtn:
			if p := pickFile(tr("Choose your GameCube disc image (USA, GCBE7D)", "Elige tu imagen del disco de GameCube (USA, GCBE7D)")); p != "" {
				setText(gui.imgEdit, p)
			}
		case idDstBtn:
			title := tr("Choose where to save the files", "Elige dónde guardar los archivos")
			if checked(gui.vita) {
				title = tr("Choose the PS Vita drive (ux0)", "Elige la unidad de la PS Vita (ux0)")
			}
			if p := pickFolder(title); p != "" {
				setText(gui.dstEdit, p)
				if !checked(gui.vita) {
					gui.pcDir = p
				}
			}
		case idVita, idPC:
			refreshDest()
		case idGo:
			if !gui.busy {
				startExtract()
			}
		case idQuit:
			procDestroyWindow.Call(hwnd)
		}
		return 0
	case msgProgress:
		procSendMessageW.Call(gui.progress, pbmSetPos, wParam, 0)
		setStatus(tr(fmt.Sprintf("Extracting and verifying… %d%%", wParam), fmt.Sprintf("Extrayendo y verificando… %d%%", wParam)))
		return 0
	case msgDone:
		finish()
		return 0
	case wmClose:
		if gui.busy && askYesNoCancel("Crash WOC", tr("Extraction in progress. Quit anyway?", "La extracción está en curso. ¿Salir de todos modos?")) != 1 {
			return 0
		}
		procDestroyWindow.Call(hwnd)
		return 0
	case wmDestroy:
		procPostQuitMessage.Call(0)
		return 0
	}
	r, _, _ := procDefWindowProcW.Call(hwnd, msg, wParam, lParam)
	return r
}

func runGUI(args []string) {
	procSetProcessDPIAware.Call()
	icc := initCommonControlsExT{size: 8, icc: 0x0020 | 0x4000} // PROGRESS_CLASS | STANDARD_CLASSES
	procInitCommonControls.Call(uintptr(unsafe.Pointer(&icc)))
	lang, _, _ := procGetUserDefaultUILng.Call()
	gui.es = lang&0x3FF == 0x0A
	hdc, _, _ := procGetDC.Call(0)
	dpi, _, _ := procGetDeviceCaps.Call(hdc, 90) // LOGPIXELSY
	procReleaseDC.Call(0, hdc)
	gui.dpi = int(dpi)
	if gui.dpi <= 0 {
		gui.dpi = 96
	}
	face := uintptr(unsafe.Pointer(utf16z("Segoe UI")))
	gui.font, _, _ = procCreateFontW.Call(uintptr(-(9 * gui.dpi / 72)), 0, 0, 0, 400, 0, 0, 0, 1, 0, 0, 5, 0, face)
	gui.bold, _, _ = procCreateFontW.Call(uintptr(-(12 * gui.dpi / 72)), 0, 0, 0, 700, 0, 0, 0, 1, 0, 0, 5, 0, face)

	exeDir := "."
	if exe, err := os.Executable(); err == nil {
		exeDir = filepath.Dir(exe)
	}
	gui.pcDir = filepath.Join(exeDir, "crashwoc-vita-data")

	hinst, _, _ := procGetModuleHandleW.Call(0)
	className := utf16z("CrashWocExtract")
	cursor, _, _ := procLoadCursorW.Call(0, 32512) // IDC_ARROW
	icon, _, _ := procLoadIconW.Call(hinst, 1)     // embedded icon, if any
	if icon == 0 {
		icon, _, _ = procLoadIconW.Call(0, 32512) // IDI_APPLICATION
	}
	brush, _, _ := procGetSysColorBrush.Call(15) // COLOR_BTNFACE
	wc := wndClassExW{
		lpfnWndProc:   syscall.NewCallback(wndProc),
		hInstance:     hinst,
		hIcon:         icon,
		hCursor:       cursor,
		hbrBackground: brush,
		lpszClassName: className,
		hIconSm:       icon,
	}
	wc.cbSize = uint32(unsafe.Sizeof(wc))
	procRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc)))

	const cw, ch = 540, 368
	style := uintptr(wsOverlapped | wsCaption | wsSysMenu | wsMinimizeBox)
	rc := rectT{0, 0, int32(px(cw)), int32(px(ch))}
	procAdjustWindowRectEx.Call(uintptr(unsafe.Pointer(&rc)), style, 0, 0)
	w, h := uintptr(rc.right-rc.left), uintptr(rc.bottom-rc.top)
	sw, _, _ := procGetSystemMetrics.Call(0)
	sh, _, _ := procGetSystemMetrics.Call(1)
	title := "Crash WOC - PS Vita " + tr("data extractor", "extractor de datos") + " " + version
	gui.hwnd, _, _ = procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(className)), uintptr(unsafe.Pointer(utf16z(title))),
		style, (sw-w)/2, (sh-h)/2, w, h, 0, 0, hinst, 0)
	ownerHwnd = gui.hwnd

	t := child("STATIC", "Crash Bandicoot: The Wrath of Cortex — PS Vita", ssLeft, 16, 12, 508, 24, 0)
	procSendMessageW.Call(t, wmSetFont, gui.bold, 1)
	child("STATIC", tr("Copies the game files from YOUR OWN GameCube disc (USA, GCBE7D).",
		"Copia los archivos del juego desde TU PROPIO disco de GameCube (USA, GCBE7D)."), ssLeft, 16, 38, 508, 34, 0)

	child("STATIC", tr("1. Disc image (.iso, .nkit.iso, .gcm, .ciso)", "1. Imagen del disco (.iso, .nkit.iso, .gcm, .ciso)"), ssLeft, 16, 88, 508, 18, 0)
	gui.imgEdit = editBox(16, 108, 404, 24, idImgEdit)
	gui.imgBtn = child("BUTTON", tr("Browse…", "Examinar…"), wsTabStop|bsPush, 428, 107, 96, 26, idImgBtn)

	child("STATIC", tr("2. Where to put the files", "2. Dónde poner los archivos"), ssLeft, 16, 146, 508, 18, 0)
	gui.vita = child("BUTTON", tr("Straight to my PS Vita (VitaShell: START > USB connection)", "Directo a mi PS Vita (VitaShell: START > Conexión USB)"),
		wsTabStop|wsGroup|bsAutoRadio, 24, 166, 500, 22, idVita)
	gui.pc = child("BUTTON", tr("To a folder on this PC (copy it to the Vita later)", "A una carpeta de este PC (la copias a la Vita después)"),
		bsAutoRadio, 24, 188, 500, 22, idPC)
	gui.dstEdit = editBox(16, 214, 404, 24, idDstEdit)
	gui.dstBtn = child("BUTTON", tr("Browse…", "Examinar…"), wsTabStop|bsPush, 428, 213, 96, 26, idDstBtn)

	gui.progress = child("msctls_progress32", "", 0, 16, 252, 508, 16, 0)
	procSendMessageW.Call(gui.progress, pbmSetRng, 0, 100) // PBM_SETRANGE32
	gui.status = child("STATIC", "", ssLeft, 16, 274, 508, 48, 0)

	gui.goBtn = child("BUTTON", tr("Extract", "Extraer"), wsTabStop|bsDefPush, 324, 328, 96, 28, idGo)
	gui.quit = child("BUTTON", tr("Close", "Cerrar"), wsTabStop|bsPush, 428, 328, 96, 28, idQuit)

	if len(args) == 1 {
		setText(gui.imgEdit, args[0])
	}
	if findVitaDrive() != "" {
		procSendMessageW.Call(gui.vita, bmSetCheck, 1, 0)
	} else {
		procSendMessageW.Call(gui.pc, bmSetCheck, 1, 0)
	}
	refreshDest()

	procShowWindow.Call(gui.hwnd, 5) // SW_SHOW
	procUpdateWindow.Call(gui.hwnd)
	var m msgT
	for {
		r, _, _ := procGetMessageW.Call(uintptr(unsafe.Pointer(&m)), 0, 0, 0)
		if int32(r) <= 0 {
			break
		}
		if d, _, _ := procIsDialogMessageW.Call(gui.hwnd, uintptr(unsafe.Pointer(&m))); d != 0 {
			continue
		}
		procTranslateMessage.Call(uintptr(unsafe.Pointer(&m)))
		procDispatchMessageW.Call(uintptr(unsafe.Pointer(&m)))
	}
}
