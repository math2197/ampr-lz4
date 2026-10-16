#pragma once
/* Cria o socket de escuta. Retorna fd >=0 ou -1 (errno, p.ex. EADDRINUSE). */
int httpd_listen(int port);
/* Laço de aceitação (não retorna). */
void httpd_serve(int listen_fd);
