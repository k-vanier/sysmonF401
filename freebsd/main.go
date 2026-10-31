package main

// sysmonf401 : envoie une fois par seconde CPU / RAM / température / débit réseau
// à l'afficheur Black Pill F401 + ST7789, sous forme d'une ligne de texte :
//   cpu=12.5 ram=43.1 temp=52.0 down=123456 up=7890 host=srv1
// Sort en erreur si l'afficheur disparaît ; rc.d (daemon -r) le relance.

import (
	"flag"
	"fmt"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"
)

func main() {
	port := flag.String("port", "", "port série (ex. /dev/cuaU0) ; détecté automatiquement si vide")
	interval := flag.Duration("interval", time.Second, "période de mise à jour")
	bright := flag.Int("brightness", -1, "rétroéclairage 0-100 à appliquer au démarrage")
	iface := flag.String("iface", "", "interfaces réseau à compter, séparées par des virgules (vide = toutes sauf lo)")
	host := flag.String("host", "", "nom affiché (par défaut : nom court de la machine)")
	noARC := flag.Bool("no-arc", false, "ne pas compter l'ARC ZFS comme RAM utilisée")
	flag.Parse()

	name := *port
	if name == "" {
		var err error
		if name, err = FindPort(); err != nil {
			fail(err)
		}
	} else if !strings.HasPrefix(name, "/") {
		name = "/dev/" + name
	}
	f, err := os.OpenFile(name, os.O_WRONLY, 0)
	if err != nil {
		fail(err)
	}
	defer f.Close()

	if *host == "" {
		h, _ := os.Hostname()
		*host, _, _ = strings.Cut(h, ".")
	}
	*host = strings.ReplaceAll(*host, " ", "_")
	var ifaces []string
	if *iface != "" {
		ifaces = strings.Split(*iface, ",")
	}

	send := func(line string) {
		if _, err := f.WriteString(line + "\n"); err != nil {
			fail(fmt.Errorf("écriture sur %s : %w", name, err))
		}
	}
	if *bright >= 0 {
		send(fmt.Sprintf("bri=%d", *bright))
	}
	fmt.Fprintf(os.Stderr, "sysmonf401 : envoi sur %s toutes les %v\n", name, *interval)

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	tick := time.NewTicker(*interval)
	defer tick.Stop()

	var cpu cpuSampler
	cpu.Sample() // première mesure : référence
	prevRx, prevTx, netOK := NetBytes(ifaces)
	prevT := time.Now()

	for {
		select {
		case <-sig:
			return
		case <-tick.C:
		}
		var b strings.Builder
		fmt.Fprintf(&b, "cpu=%.1f", cpu.Sample())
		if r, ok := MemoryPct(*noARC); ok {
			fmt.Fprintf(&b, " ram=%.1f", r)
		}
		if t, ok := CPUTemp(); ok {
			fmt.Fprintf(&b, " temp=%.1f", t)
		}
		rx, tx, ok := NetBytes(ifaces)
		now := time.Now()
		// Compteurs qui reculent (interface recréée) : on saute cette mesure.
		if ok && netOK && rx >= prevRx && tx >= prevTx {
			dt := now.Sub(prevT).Seconds()
			fmt.Fprintf(&b, " down=%.0f up=%.0f", float64(rx-prevRx)/dt, float64(tx-prevTx)/dt)
		}
		prevRx, prevTx, netOK, prevT = rx, tx, ok, now
		fmt.Fprintf(&b, " host=%s", *host)
		send(b.String())
	}
}

func fail(err error) {
	fmt.Fprintln(os.Stderr, "sysmonf401 :", err)
	os.Exit(1)
}
