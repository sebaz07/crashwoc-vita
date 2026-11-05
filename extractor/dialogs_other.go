//go:build !windows

package main

const dialogsAvailable = false

func pickFile(title string) string          { return "" }
func pickFolder(title string) string        { return "" }
func askYesNoCancel(title, text string) int { return -1 }
func showInfo(title, text string)           {}
func bringConsoleToFront()                  {}
