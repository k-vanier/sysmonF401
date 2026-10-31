/* Afficheur CPU / RAM / température / réseau : réception USB et dessin. */
#ifndef SYSMON_H
#define SYSMON_H

#include <stdint.h>

void sysmon_init(void);
void sysmon_loop(void);
/* Appelée par CDC_Receive_FS (interruption USB) avec chaque paquet reçu. */
void sysmon_rx(const uint8_t *buf, uint32_t len);

#endif
