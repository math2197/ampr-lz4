/* Atalho "AMPR-LZ4" na aba Mídia do PS5 (app AMLZ00001 que abre a web UI). No host é só um log. */
#pragma once

/* Dispara a instalação numa thread destacada (pilha padrão). Retorna 0 se a thread foi criada
 * (ou se g_cfg.shortcut == 0, caso em que nada é feito). */
int shortcut_start(void);
