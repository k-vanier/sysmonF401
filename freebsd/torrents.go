package main

// Résumé des torrents écrit par c411_transmission (c411.py, build_status_line)
// dans /var/log/torrents_status, une ligne réécrite à chaque passage, ex. :
//   DL 2/3 (87% 45%) +1 att | libre 87G dispo 29G | env 44.1T rec 28.0T r1.58 1err
//     | top 6.99 Groot's Firs | flop 0.04 Nightcrawler | DL complet
// Variantes : « manque 12G » au lieu de « dispo », « r- », top / flop sans nom
// (« top 6.95 flop 0.07 »), ligne d'erreur « libre 98G | ERREUR message ».
//
// TorrentLine la traduit en clés pour l'afficheur (espaces remplacés par _) :
//   tdl=2 tmax=3 tpct=87,45 tatt=1 tfree=87G tavail=29G tup=44.1T tdown=28.0T
//   tratio=1.58 terr=1 ttopr=6.99 ttopn=Groot's_Firs tflopr=0.04 tflopn=Nightcrawler
//   tact=DL_complet tage=720

import (
	"fmt"
	"os"
	"regexp"
	"strings"
	"time"
)

var (
	reDL     = regexp.MustCompile(`^DL (\d+)(?:/(\d+))?(?: \(([^)]*)\))?(?: \+(\d+) att)?`)
	reSpace  = regexp.MustCompile(`^libre (\S+)(?: (dispo|manque) (\S+))?`)
	reTotals = regexp.MustCompile(`^env (\S+) rec (\S+) r(\S+)(?: (\d+)err)?`)
	reRank   = regexp.MustCompile(`^(top|flop) (\S+)(?: (.*))?$`)
)

// TorrentLine lit le résumé et renvoie la ligne de clés ; ok=false si le fichier
// est absent ou vide.
func TorrentLine(path string) (string, bool) {
	fi, err := os.Stat(path)
	if err != nil {
		return "", false
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return "", false
	}
	var line string
	for _, l := range strings.Split(string(data), "\n") {
		if l = strings.TrimSpace(l); l != "" {
			line = l // la dernière ligne non vide
		}
	}
	if line == "" {
		return "", false
	}

	var kv []string
	add := func(k, v string) {
		if v = strings.TrimSpace(v); v != "" {
			kv = append(kv, k+"="+strings.ReplaceAll(v, " ", "_"))
		}
	}
	// top / flop : « top R nom », ou « top R flop R » quand les noms n'ont pas tenu.
	var rank func(p string)
	rank = func(p string) {
		m := reRank.FindStringSubmatch(p)
		if m == nil {
			return
		}
		add("t"+m[1]+"r", m[2])
		rest := m[3]
		if m[1] == "top" && strings.HasPrefix(rest, "flop ") {
			rank(rest)
			return
		}
		add("t"+m[1]+"n", rest)
	}

	parts := strings.Split(line, " | ")
	for i, p := range parts {
		p = strings.TrimSpace(p)
		switch {
		case i > 0 && i == len(parts)-1 && !strings.HasPrefix(p, "ERREUR"):
			// ce que le passage a fait : RAS, DL complet, +nom... (avant le cas DL :
			// « DL complet » commence aussi par DL)
			add("tact", p)
		case strings.HasPrefix(p, "DL "):
			if m := reDL.FindStringSubmatch(p); m != nil {
				add("tdl", m[1])
				add("tmax", m[2])
				add("tpct", strings.Join(strings.Fields(strings.ReplaceAll(m[3], "%", "")), ","))
				add("tatt", m[4])
			}
		case strings.HasPrefix(p, "libre "):
			if m := reSpace.FindStringSubmatch(p); m != nil {
				add("tfree", m[1])
				if m[2] == "dispo" {
					add("tavail", m[3])
				} else {
					add("tneed", m[3])
				}
			}
		case strings.HasPrefix(p, "env "):
			if m := reTotals.FindStringSubmatch(p); m != nil {
				add("tup", m[1])
				add("tdown", m[2])
				add("tratio", m[3])
				add("terr", m[4])
			}
		case strings.HasPrefix(p, "top "), strings.HasPrefix(p, "flop "):
			rank(p)
		case strings.HasPrefix(p, "ERREUR"):
			add("terror", strings.TrimPrefix(strings.TrimPrefix(p, "ERREUR"), " "))
		}
	}
	add("tage", fmt.Sprintf("%.0f", time.Since(fi.ModTime()).Seconds()))
	return strings.Join(kv, " "), true
}
