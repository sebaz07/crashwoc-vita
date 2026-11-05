//go:build windows

// Native Windows dialogs (no cgo): pick the disc image, ask where to put the
// files, pick a folder (for example the Vita's memory card shown as a drive
// by VitaShell's USB mode).
package main

import (
	"runtime"
	"syscall"
	"unsafe"
)

var (
	comdlg32 = syscall.NewLazyDLL("comdlg32.dll")
	shell32  = syscall.NewLazyDLL("shell32.dll")
	user32   = syscall.NewLazyDLL("user32.dll")
	ole32    = syscall.NewLazyDLL("ole32.dll")
	kernel32 = syscall.NewLazyDLL("kernel32.dll")

	procGetOpenFileNameW     = comdlg32.NewProc("GetOpenFileNameW")
	procSHBrowseForFolderW   = shell32.NewProc("SHBrowseForFolderW")
	procSHGetPathFromIDListW = shell32.NewProc("SHGetPathFromIDListW")
	procMessageBoxW          = user32.NewProc("MessageBoxW")
	procSetForegroundWindow  = user32.NewProc("SetForegroundWindow")
	procCoInitializeEx       = ole32.NewProc("CoInitializeEx")
	procCoTaskMemFree        = ole32.NewProc("CoTaskMemFree")
	procGetConsoleWindow     = kernel32.NewProc("GetConsoleWindow")
)

func init() {
	// shell dialogs need a single-threaded COM apartment on one OS thread
	runtime.LockOSThread()
}

type openFileNameW struct {
	lStructSize       uint32
	hwndOwner         uintptr
	hInstance         uintptr
	lpstrFilter       *uint16
	lpstrCustomFilter *uint16
	nMaxCustFilter    uint32
	nFilterIndex      uint32
	lpstrFile         *uint16
	nMaxFile          uint32
	lpstrFileTitle    *uint16
	nMaxFileTitle     uint32
	lpstrInitialDir   *uint16
	lpstrTitle        *uint16
	flags             uint32
	nFileOffset       uint16
	nFileExtension    uint16
	lpstrDefExt       *uint16
	lCustData         uintptr
	lpfnHook          uintptr
	lpTemplateName    *uint16
	pvReserved        uintptr
	dwReserved        uint32
	flagsEx           uint32
}

type browseInfoW struct {
	hwndOwner      uintptr
	pidlRoot       uintptr
	pszDisplayName *uint16
	lpszTitle      *uint16
	ulFlags        uint32
	lpfn           uintptr
	lParam         uintptr
	iImage         int32
}

const dialogsAvailable = true

// ownerHwnd: the window dialogs belong to (the extractor window when shown)
var ownerHwnd uintptr

func consoleWindow() uintptr {
	if ownerHwnd != 0 {
		return ownerHwnd
	}
	h, _, _ := procGetConsoleWindow.Call()
	return h
}

func utf16z(s string) *uint16 {
	p, _ := syscall.UTF16PtrFromString(s)
	return p
}

// filter: pairs of (label, pattern), joined with NULs and ending in two NULs
func filterString(pairs ...string) *uint16 {
	var u []uint16
	for _, s := range pairs {
		u = append(u, syscall.StringToUTF16(s)...) // includes the NUL
	}
	u = append(u, 0)
	return &u[0]
}

// pickFile shows the Open dialog; "" when cancelled.
func pickFile(title string) string {
	buf := make([]uint16, 32768)
	ofn := openFileNameW{
		hwndOwner:    consoleWindow(),
		lpstrFilter:  filterString("GameCube disc image (*.iso, *.gcm, *.ciso)", "*.iso;*.gcm;*.ciso", "All files / Todos los archivos (*.*)", "*.*"),
		nFilterIndex: 1,
		lpstrFile:    &buf[0],
		nMaxFile:     uint32(len(buf)),
		lpstrTitle:   utf16z(title),
		flags:        0x00001000 | 0x00000800 | 0x00080000, // FILEMUSTEXIST | PATHMUSTEXIST | EXPLORER
	}
	ofn.lStructSize = uint32(unsafe.Sizeof(ofn))
	r, _, _ := procGetOpenFileNameW.Call(uintptr(unsafe.Pointer(&ofn)))
	if r == 0 {
		return ""
	}
	return syscall.UTF16ToString(buf)
}

// pickFolder shows the folder browser; "" when cancelled.
func pickFolder(title string) string {
	procCoInitializeEx.Call(0, 2) // COINIT_APARTMENTTHREADED
	name := make([]uint16, 260)
	bi := browseInfoW{
		hwndOwner:      consoleWindow(),
		pszDisplayName: &name[0],
		lpszTitle:      utf16z(title),
		ulFlags:        0x0001 | 0x0040, // RETURNONLYFSDIRS | NEWDIALOGSTYLE
	}
	pidl, _, _ := procSHBrowseForFolderW.Call(uintptr(unsafe.Pointer(&bi)))
	if pidl == 0 {
		return ""
	}
	defer procCoTaskMemFree.Call(pidl)
	path := make([]uint16, 32768)
	r, _, _ := procSHGetPathFromIDListW.Call(pidl, uintptr(unsafe.Pointer(&path[0])))
	if r == 0 {
		return ""
	}
	return syscall.UTF16ToString(path)
}

// askYesNoCancel: 1 yes, 0 no, -1 cancel
func askYesNoCancel(title, text string) int {
	r, _, _ := procMessageBoxW.Call(consoleWindow(), uintptr(unsafe.Pointer(utf16z(text))),
		uintptr(unsafe.Pointer(utf16z(title))), 0x3|0x20|0x00010000) // YESNOCANCEL | ICONQUESTION | SETFOREGROUND
	switch r {
	case 6:
		return 1
	case 7:
		return 0
	}
	return -1
}

func showInfo(title, text string) {
	procMessageBoxW.Call(consoleWindow(), uintptr(unsafe.Pointer(utf16z(text))),
		uintptr(unsafe.Pointer(utf16z(title))), 0x40|0x00010000) // ICONINFORMATION | SETFOREGROUND
}

func bringConsoleToFront() {
	procSetForegroundWindow.Call(consoleWindow())
}
