tcc -Wl,-subsystem=windows -o singbox_tray.exe singbox_tray_switch.c -lshell32 -ladvapi32 -lwininet >log.txt 2>&1
