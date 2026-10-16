/* Contrato entre o núcleo de conversão (convert.c e afins) e a plataforma (main/httpd/smp).
 * Não alterar sem combinar com os dois lados. */
#pragma once
#include <stdint.h>
#include <stdatomic.h>

typedef enum {
  CONV_STAGE_IDLE = 0,
  CONV_STAGE_MOUNT,    /* plataforma: montagem via SMP */
  CONV_STAGE_SCAN,
  CONV_STAGE_PACK,
  CONV_STAGE_COPY,
  CONV_STAGE_VERIFY,
  CONV_STAGE_FINISH,   /* plataforma: fakelib + desmontar */
  CONV_STAGE_DONE,
  CONV_STAGE_ERROR,
  CONV_STAGE_CANCELLED
} conv_stage_t;

/* Engine detectada na varredura (perfil de classificação loose/pack). */
typedef enum {
  CONV_ENGINE_UNKNOWN = 0,
  CONV_ENGINE_UNREAL,      /* .utoc/.ucas ou .pak em .../Paks/ */
  CONV_ENGINE_UNITY,       /* Media/ com .assets/.resS, ou globalgamemanagers */
  CONV_ENGINE_NORTHLIGHT   /* .rmdtoc (Remedy: Control, Alan Wake 2) */
} conv_engine_t;

/* Nome estável (API/JSON) da engine: "unknown", "unreal", "unity", "northlight". */
static inline const char *conv_engine_name(int e) {
  switch (e) {
    case CONV_ENGINE_UNREAL: return "unreal";
    case CONV_ENGINE_UNITY: return "unity";
    case CONV_ENGINE_NORTHLIGHT: return "northlight";
    default: return "unknown";
  }
}

typedef struct {
  _Atomic int      stage;          /* conv_stage_t */
  _Atomic uint64_t bytes_total;    /* bytes lógicos a processar na etapa atual */
  _Atomic uint64_t bytes_done;
  _Atomic uint64_t src_bytes;      /* tamanho total da origem (lógico) */
  _Atomic uint64_t out_bytes;      /* tamanho final no destino */
  _Atomic int      cancel;         /* plataforma seta 1 para cancelar */
  /* Estimativa publicada ao fim do SCAN (antes do auto-loose da etapa PACK). */
  _Atomic int      engine;         /* conv_engine_t */
  _Atomic uint64_t packed_bytes;   /* bytes lógicos destinados aos volumes .pak */
  _Atomic uint64_t loose_bytes;    /* bytes lógicos copiados soltos */
  /* Escrito na etapa PACK: bytes que o auto-loose (arquivos >= 64 MiB sem ganho na amostragem) tirou de
   * packed_bytes. Real nos pacotes = packed_bytes - auto_loose_bytes; real solto = loose_bytes + auto_loose_bytes. */
  _Atomic uint64_t auto_loose_bytes;
  /* Perfil embutido aplicado (profiles/index.json), escrito antes de packed/loose_bytes no fim do SCAN:
   * leia só depois de ver packed_bytes + loose_bytes > 0. "" = nenhum. */
  char             profile[32];
  /* 1 se o jogo importa libScePlayGo e a origem não tem fakelib/libScePlayGo.sprx (a plataforma confere o
   * SMP antes de avisar). Escrito no fim do SCAN. */
  _Atomic int      playgo_missing;
  char             error[256];     /* mensagem quando stage == ERROR */
} conv_progress_t;

typedef struct {
  const char *src_dir;      /* raiz do jogo montado (contém eboot.bin, sce_sys/) */
  const char *dst_dir;      /* pasta final, ex.: /data/homebrew/PPSA01234-AMPR (não pode existir) */
  const char *fakelib_sprx; /* caminho do libSceAmpr.sprx a copiar para fakelib/, ou NULL */
  int         level;        /* LZ4: 1..12 (1-4 fast, 5-12 HC) */
  int         workers;      /* 1..16 */
  /* Regras extras de "manter solto", além do perfil da engine (NULL/"" = nenhuma). Lista separada por
   * vírgulas, sem distinção de maiúsculas: ".ucas" (extensão), "global.ucas" (nome do arquivo) ou
   * "Content/Paks/global.ucas" / "Content/Movies/" (prefixo do caminho relativo, contém '/'). */
  const char *keep_loose;
  /* 1 = desliga o auto-loose da etapa PACK: arquivos grandes vão para os pacotes mesmo sem ganho de tamanho
   * na amostragem (as regras de segurança, keep_loose, perfil e engine continuam valendo). 0 = padrão. */
  int         pack_all;
} conv_opts_t;

/* Checa se o jogo em src_dir importa libSceAmpr e não é AMPR já convertido.
 * Retorna 0 se compatível; senão !=0 e escreve o motivo em why. */
int conv_check_source(const char *src_dir, char *why, int why_len);

/* Converte src_dir -> dst_dir (AMPR). Bloqueante; atualiza *p. Em erro ou cancelamento
 * remove dst_dir. Retorna 0 em sucesso. */
int conv_run(const conv_opts_t *o, conv_progress_t *p);

/* Notificação no PS5 (printf no host). Implementada em util.c. */
void conv_notify(const char *fmt, ...);
/* Se não NULL, recebe cada mensagem de conv_notify antes do envio (a plataforma usa para o log). */
extern void (*conv_notify_hook)(const char *msg);
