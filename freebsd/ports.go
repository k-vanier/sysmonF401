package main

// Recherche du port série de l'afficheur : le firmware se présente en CDC ACM
// (pilote umodem) avec le VID/PID STMicroelectronics 0483:5740.
// On lit dev.umodem.<n>.%pnpinfo et dev.umodem.<n>.ttyname, comme miniscreen.

import (
	"errors"
	"fmt"
	"regexp"
	"strings"

	"golang.org/x/sys/unix"
)

var (
	reVendor  = regexp.MustCompile(`vendor=0x([0-9a-fA-F]{4})`)
	reProduct = regexp.MustCompile(`product=0x([0-9a-fA-F]{4})`)
)

// FindPort renvoie le premier /dev/cuaU* dont le VID/PID est celui du firmware.
func FindPort() (string, error) {
	for n := 0; n < 32; n++ { // les numéros d'unité peuvent avoir des trous
		base := fmt.Sprintf("dev.umodem.%d.", n)
		tty, err := unix.Sysctl(base + "ttyname")
		if err != nil || tty == "" {
			continue
		}
		pnp, _ := unix.Sysctl(base + "%pnpinfo")
		v, p := reVendor.FindStringSubmatch(pnp), reProduct.FindStringSubmatch(pnp)
		if v != nil && p != nil && strings.EqualFold(v[1], "0483") && strings.EqualFold(p[1], "5740") {
			return "/dev/cua" + tty, nil
		}
	}
	return "", errors.New("afficheur introuvable (VID/PID 0483:5740 sur umodem) ; branché ? kldload umodem ?")
}
