/*
 * Simulador de escalonamento multiprocessado com paginacao sob demanda (M2 - SO).
 *
 * Uso:
 *   ./simulador --arquivo cenarios/cenario_A_t0.json --cpus 4 --sched rr --quantum 10
 *   ./simulador                 (sem argumentos: abre o menu interativo)
 *
 * Opcoes da secao 8: --arquivo --cpus --sched --quantum --subst --quadros --custo-troca --log
 * Extras: --custo-fault N   tempo de um page fault (padrao 20)
 *         --log-ate T       grava o log so ate o instante T (padrao 2000; 0 = tudo)
 *         --ao-vivo         mostra a simulacao acontecendo, unidade por unidade
 *         --atraso MS       pausa entre unidades no modo ao vivo (padrao 150 ms)
 *         --saida DIR       pasta dos arquivos gerados (padrao saida)
 *
 * JSON lido com a biblioteca cJSON (lib/cJSON.c, licenca MIT).
 */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "lib/cJSON.h"

#define JANELA_GANTT 200
#define SEM_PID (-1000000)

enum { FCFS, SJF, RR };
enum { SUBST_FIFO, SUBST_LRU };
enum { MOT_ES, MOT_FAULT, MOT_RETRY };

/* ------------------------------------------------------------------------- */
/* Estruturas                                                                 */
/* ------------------------------------------------------------------------- */
typedef struct {
    int cpu;    /* > 0: burst de CPU com 'cpu' unidades */
    int es;     /* > 0: burst de E/S com 'es' unidades */
    int *refs;  /* uma pagina por unidade de CPU; NULL = burst sem referencias */
} Burst;

typedef struct {
    int pid, chegada, paginas;
    char classe[16];
    int nb;
    Burst *b;
    int etapa;          /* indice em b */
    int pos;            /* unidades ja executadas do burst de CPU atual */
    long espera;
    int entrou_fila, primeiro_despacho, fim;
    int npag;           /* tamanho da tabela de paginas */
    int *quadro;        /* tabela de paginas: quadro[pag] = indice do quadro ou -1 */
} Proc;

typedef struct {
    int proc;           /* indice do processo na CPU, -1 = livre */
    int ultimo;         /* pid do ultimo processo que executou aqui */
    int troca_fim;      /* instante em que termina a troca em curso, -1 = sem troca */
    int executou;       /* executou a unidade [t-1, t) */
    int quantum_usado;
    long util, troca, ocioso, n_trocas;
    int gantt_est[JANELA_GANTT], gantt_pid[JANELA_GANTT];
} CPU;

/* Fila de prontos: FCFS/RR = fila circular; SJF = heap por (restante, ordem de entrada) */
typedef struct { int restante; long seq; int proc; } ItemSJF;
typedef struct {
    int sjf, n, cap, ini;
    int *circ;
    ItemSJF *heap;
    long seq;
} Fila;

/* Bloqueados: heap por (retorno, pid) */
typedef struct { int ret, pid, motivo, proc; } ItemBloq;
typedef struct { ItemBloq *v; int n; } Bloq;

/* Memoria: quadros numa lista duplamente ligada na ordem de substituicao.
 * Cabeca = proxima vitima. FIFO: insere no fim e acerto nao mexe.
 * LRU: acerto move para o fim. Vitima = primeiro nao protegido a partir da cabeca. */
typedef struct { int proc, pag, prot, prev, next; } Quadro;
typedef struct {
    int n, usados, cab, cauda, livre, lru;
    Quadro *q;
} Memoria;

static Proc *P;
static int NP;

/* ------------------------------------------------------------------------- */
/* Utilidades                                                                 */
/* ------------------------------------------------------------------------- */
static void *xmalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) { fprintf(stderr, "sem memoria\n"); exit(1); }
    return p;
}

static void morre(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

/* ------------------------------------------------------------------------- */
/* Leitura do JSON (cJSON)                                                    */
/* ------------------------------------------------------------------------- */
static int cmp_chegada(const void *a, const void *b) {
    const Proc *x = a, *y = b;
    if (x->chegada != y->chegada) return x->chegada - y->chegada;
    return x->pid - y->pid;
}

static void carrega_json(const char *arq) {
    FILE *f = fopen(arq, "rb");
    if (!f) morre("nao abriu %s", arq);
    fseek(f, 0, SEEK_END);
    long tam = ftell(f);
    rewind(f);
    char *txt = xmalloc(tam + 1);
    if (fread(txt, 1, tam, f) != (size_t)tam) morre("erro lendo %s", arq);
    fclose(f);

    cJSON *raiz = cJSON_Parse(txt);
    free(txt);
    if (!cJSON_IsArray(raiz)) morre("%s: esperado uma lista de processos", arq);

    NP = cJSON_GetArraySize(raiz);
    P = xmalloc(sizeof(Proc) * NP);
    int i = 0;
    cJSON *jp;
    cJSON_ArrayForEach(jp, raiz) {
        Proc *p = &P[i++];
        p->pid = cJSON_GetObjectItem(jp, "pid")->valueint;
        p->chegada = cJSON_GetObjectItem(jp, "chegada")->valueint;
        cJSON *c = cJSON_GetObjectItem(jp, "classe");
        snprintf(p->classe, sizeof p->classe, "%s", cJSON_IsString(c) ? c->valuestring : "-");
        cJSON *pg = cJSON_GetObjectItem(jp, "paginas");
        p->paginas = cJSON_IsNumber(pg) ? pg->valueint : 0;
        p->npag = p->paginas;

        cJSON *bs = cJSON_GetObjectItem(jp, "bursts");
        p->nb = cJSON_GetArraySize(bs);
        p->b = xmalloc(sizeof(Burst) * p->nb);
        int k = 0;
        cJSON *jb;
        cJSON_ArrayForEach(jb, bs) {
            Burst *b = &p->b[k++];
            cJSON *cpu = cJSON_GetObjectItem(jb, "cpu");
            if (cJSON_IsNumber(cpu)) {
                b->cpu = cpu->valueint;
                cJSON *refs = cJSON_GetObjectItem(jb, "refs");
                if (cJSON_IsArray(refs) && cJSON_GetArraySize(refs) > 0) {
                    if (cJSON_GetArraySize(refs) < b->cpu) morre("pid %d: refs menor que cpu", p->pid);
                    b->refs = xmalloc(sizeof(int) * b->cpu);
                    for (int r = 0; r < b->cpu; r++) {
                        b->refs[r] = cJSON_GetArrayItem(refs, r)->valueint;
                        if (b->refs[r] + 1 > p->npag) p->npag = b->refs[r] + 1;
                    }
                }
            } else {
                b->es = cJSON_GetObjectItem(jb, "es")->valueint;
            }
        }
        p->quadro = xmalloc(sizeof(int) * (p->npag ? p->npag : 1));
        for (int g = 0; g < p->npag; g++) p->quadro[g] = -1;
    }
    cJSON_Delete(raiz);
    qsort(P, NP, sizeof(Proc), cmp_chegada);
}

/* ------------------------------------------------------------------------- */
/* Fila de prontos                                                            */
/* ------------------------------------------------------------------------- */
static int sjf_menor(ItemSJF a, ItemSJF b) {
    return a.restante != b.restante ? a.restante < b.restante : a.seq < b.seq;
}

static void fila_entra(Fila *f, int i, int t) {
    P[i].entrou_fila = t;
    if (!f->sjf) {
        f->circ[(f->ini + f->n) % f->cap] = i;
        f->n++;
        return;
    }
    Proc *p = &P[i];
    ItemSJF it = { p->b[p->etapa].cpu - p->pos, f->seq++, i };
    int k = f->n++;
    while (k > 0 && sjf_menor(it, f->heap[(k - 1) / 2])) {
        f->heap[k] = f->heap[(k - 1) / 2];
        k = (k - 1) / 2;
    }
    f->heap[k] = it;
}

static int fila_sai(Fila *f) {
    if (!f->sjf) {
        int i = f->circ[f->ini];
        f->ini = (f->ini + 1) % f->cap;
        f->n--;
        return i;
    }
    int topo = f->heap[0].proc;
    ItemSJF ult = f->heap[--f->n];
    int k = 0;
    for (;;) {
        int m = 2 * k + 1;
        if (m >= f->n) break;
        if (m + 1 < f->n && sjf_menor(f->heap[m + 1], f->heap[m])) m++;
        if (!sjf_menor(f->heap[m], ult)) break;
        f->heap[k] = f->heap[m];
        k = m;
    }
    f->heap[k] = ult;
    return topo;
}

static int cmp_sjf(const void *a, const void *b) {
    return sjf_menor(*(const ItemSJF *)a, *(const ItemSJF *)b) ? -1 : 1;
}

/* pids da fila em ordem de atendimento (para log e modo ao vivo) */
static int fila_pids(Fila *f, int *out) {
    if (!f->sjf) {
        for (int k = 0; k < f->n; k++) out[k] = P[f->circ[(f->ini + k) % f->cap]].pid;
        return f->n;
    }
    ItemSJF *tmp = xmalloc(sizeof(ItemSJF) * f->n);
    memcpy(tmp, f->heap, sizeof(ItemSJF) * f->n);
    qsort(tmp, f->n, sizeof(ItemSJF), cmp_sjf);
    for (int k = 0; k < f->n; k++) out[k] = P[tmp[k].proc].pid;
    free(tmp);
    return f->n;
}

/* ------------------------------------------------------------------------- */
/* Bloqueados                                                                 */
/* ------------------------------------------------------------------------- */
static int bloq_menor(ItemBloq a, ItemBloq b) {
    return a.ret != b.ret ? a.ret < b.ret : a.pid < b.pid;
}

static void bloq_poe(Bloq *h, int i, int ret, int motivo) {
    ItemBloq it = { ret, P[i].pid, motivo, i };
    int k = h->n++;
    while (k > 0 && bloq_menor(it, h->v[(k - 1) / 2])) {
        h->v[k] = h->v[(k - 1) / 2];
        k = (k - 1) / 2;
    }
    h->v[k] = it;
}

static ItemBloq bloq_tira(Bloq *h) {
    ItemBloq topo = h->v[0], ult = h->v[--h->n];
    int k = 0;
    for (;;) {
        int m = 2 * k + 1;
        if (m >= h->n) break;
        if (m + 1 < h->n && bloq_menor(h->v[m + 1], h->v[m])) m++;
        if (!bloq_menor(h->v[m], ult)) break;
        h->v[k] = h->v[m];
        k = m;
    }
    h->v[k] = ult;
    return topo;
}

/* ------------------------------------------------------------------------- */
/* Memoria                                                                    */
/* ------------------------------------------------------------------------- */
static void mem_desliga(Memoria *m, int f) {
    Quadro *q = &m->q[f];
    if (q->prev >= 0) m->q[q->prev].next = q->next; else m->cab = q->next;
    if (q->next >= 0) m->q[q->next].prev = q->prev; else m->cauda = q->prev;
}

static void mem_poe_fim(Memoria *m, int f) {
    m->q[f].prev = m->cauda;
    m->q[f].next = -1;
    if (m->cauda >= 0) m->q[m->cauda].next = f; else m->cab = f;
    m->cauda = f;
}

static void mem_libera_quadro(Memoria *m, int f) {
    mem_desliga(m, f);
    P[m->q[f].proc].quadro[m->q[f].pag] = -1;
    m->q[f].next = m->livre;
    m->livre = f;
    m->usados--;
}

/* acerto: tira a protecao (a referencia foi feita) e, no LRU, move para o fim */
static int mem_acesso(Memoria *m, int i, int pag) {
    int f = P[i].quadro[pag];
    if (f < 0) return 0;
    m->q[f].prot = 0;
    if (m->lru && f != m->cauda) { mem_desliga(m, f); mem_poe_fim(m, f); }
    return 1;
}

/* poe a pagina num quadro (protegida); retorna 0 se nao ha vitima */
static int mem_carrega(Memoria *m, int i, int pag) {
    if (m->usados >= m->n) {
        int v = m->cab;
        while (v >= 0 && m->q[v].prot) v = m->q[v].next;
        if (v < 0) return 0;
        mem_libera_quadro(m, v);
    }
    int f = m->livre;
    m->livre = m->q[f].next;
    m->q[f].proc = i;
    m->q[f].pag = pag;
    m->q[f].prot = 1;
    mem_poe_fim(m, f);
    P[i].quadro[pag] = f;
    m->usados++;
    return 1;
}

static void mem_libera_proc(Memoria *m, int i) {
    for (int g = 0; g < P[i].npag; g++)
        if (P[i].quadro[g] >= 0) mem_libera_quadro(m, P[i].quadro[g]);
}

/* ------------------------------------------------------------------------- */
/* Eventos do instante (log e modo ao vivo)                                   */
/* ------------------------------------------------------------------------- */
static char **EV;
static int NEV, CAPEV, GUARDA_EV;

static void evento(const char *fmt, ...) {
    if (!GUARDA_EV) return;
    if (NEV == CAPEV) {
        CAPEV = CAPEV ? 2 * CAPEV : 64;
        EV = realloc(EV, sizeof(char *) * CAPEV);
        for (int k = NEV; k < CAPEV; k++) EV[k] = xmalloc(96);
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(EV[NEV++], 96, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------------- */
/* Parametros e resultado                                                     */
/* ------------------------------------------------------------------------- */
typedef struct {
    char arquivo[512], saida[256];
    int cpus, sched, quantum, subst, quadros, custo_troca, custo_fault;
    int log, log_ate, ao_vivo, atraso;
} Params;

static const char *NOME_SCHED[] = { "fcfs", "sjf", "rr" };
static const char *NOME_SUBST[] = { "fifo", "lru" };

static volatile sig_atomic_t ao_vivo_ligado = 0, saiu_ao_vivo = 0;

static void trata_sigint(int s) {
    (void)s;
    if (ao_vivo_ligado) { ao_vivo_ligado = 0; saiu_ao_vivo = 1; return; }
    signal(SIGINT, SIG_DFL);
    raise(SIGINT);
}

static const char *estado_cpu(CPU *c, char *buf) {
    if (c->proc < 0) return "OCIOSA";
    if (c->troca_fim >= 0) return "TROCA";
    sprintf(buf, "P%d", P[c->proc].pid);
    return buf;
}


/* bloqueados em ordem de pid (log) ou de retorno (ao vivo) */
static int cmp_bloq_pid(const void *a, const void *b) {
    return ((const ItemBloq *)a)->pid - ((const ItemBloq *)b)->pid;
}
static int cmp_bloq_ret(const void *a, const void *b) {
    return bloq_menor(*(const ItemBloq *)a, *(const ItemBloq *)b) ? -1 : 1;
}

static void painel(int t, CPU *cpu, int ncpu, Fila *fila, Bloq *bloq, Memoria *mem,
                   int terminados, long faults, int *buf) {
    char e[32];
    printf("\033[H\033[J=== SIMULADOR AO VIVO ===   t = %d   (Ctrl+C sai do modo ao vivo)\n\n", t);
    for (int k = 0; k < ncpu; k++)
        printf("  CPU %d : %-8s  util=%ld troca=%ld ocioso=%ld\n", k, estado_cpu(&cpu[k], e),
               cpu[k].util, cpu[k].troca, cpu[k].ocioso);
    int n = fila_pids(fila, buf);
    printf("\n  Prontos (%d):", n);
    for (int k = 0; k < n && k < 20; k++) printf(" P%d", buf[k]);
    printf("%s\n", n > 20 ? " ..." : "");
    ItemBloq *tmp = xmalloc(sizeof(ItemBloq) * (bloq->n + 1));
    memcpy(tmp, bloq->v, sizeof(ItemBloq) * bloq->n);
    qsort(tmp, bloq->n, sizeof(ItemBloq), cmp_bloq_ret);
    printf("  Bloqueados (%d):", bloq->n);
    for (int k = 0; k < bloq->n && k < 12; k++) printf(" P%d->%d", tmp[k].pid, tmp[k].ret);
    printf("%s\n", bloq->n > 12 ? " ..." : "");
    free(tmp);
    printf("  Quadros: %d/%d   Page faults: %ld   Terminados: %d/%d\n\n", mem->usados, mem->n,
           faults, terminados, NP);
    printf("  Eventos em t=%d:\n", t);
    for (int k = 0; k < NEV && k < 10; k++) printf("    %s\n", EV[k]);
    if (!NEV) printf("    -\n");
    fflush(stdout);
}

static void escreve_log(FILE *log, int t, CPU *cpu, int ncpu, Fila *fila, Bloq *bloq,
                        Memoria *mem, int *buf) {
    char e[32];
    fprintf(log, "t=%d\n", t);
    for (int k = 0; k < NEV; k++) fprintf(log, "  evento: %s\n", EV[k]);
    int n = fila_pids(fila, buf);
    fprintf(log, "  prontos: [");
    for (int k = 0; k < n; k++) fprintf(log, "%s%d", k ? ", " : "", buf[k]);
    fprintf(log, "]\n  bloqueados:");
    ItemBloq *tmp = xmalloc(sizeof(ItemBloq) * (bloq->n + 1));
    memcpy(tmp, bloq->v, sizeof(ItemBloq) * bloq->n);
    qsort(tmp, bloq->n, sizeof(ItemBloq), cmp_bloq_pid);
    for (int k = 0; k < bloq->n; k++)
        fprintf(log, " P%d->%d%s", tmp[k].pid, tmp[k].ret, tmp[k].motivo != MOT_ES ? "(fault)" : "");
    free(tmp);
    fprintf(log, "\n  cpus:");
    for (int k = 0; k < ncpu; k++) fprintf(log, " cpu%d=%s", k, estado_cpu(&cpu[k], e));
    fprintf(log, "\n  quadros: %d/%d\n", mem->usados, mem->n);
}

/* ------------------------------------------------------------------------- */
/* Simulacao                                                                  */
/* ------------------------------------------------------------------------- */
typedef struct {
    CPU *cpu;
    int makespan;
    long faults, faults_sem_vitima, referencias;
} Resultado;

static Resultado simular(Params *a, FILE *log) {
    int ncpu = a->cpus;
    CPU *cpu = xmalloc(sizeof(CPU) * ncpu);
    for (int k = 0; k < ncpu; k++) {
        cpu[k].proc = -1;
        cpu[k].ultimo = SEM_PID;
        cpu[k].troca_fim = -1;
    }
    Fila fila = { .sjf = a->sched == SJF, .cap = NP };
    fila.circ = xmalloc(sizeof(int) * NP);
    fila.heap = xmalloc(sizeof(ItemSJF) * NP);
    Bloq bloq = { xmalloc(sizeof(ItemBloq) * NP), 0 };
    Memoria mem = { .n = a->quadros, .cab = -1, .cauda = -1, .lru = a->subst == SUBST_LRU };
    mem.q = xmalloc(sizeof(Quadro) * a->quadros);
    for (int f = 0; f < a->quadros; f++) mem.q[f].next = f + 1 < a->quadros ? f + 1 : -1;
    mem.livre = 0;

    int *entrando = xmalloc(sizeof(int) * NP), *ent_mot = xmalloc(sizeof(int) * NP);
    int *expirados = xmalloc(sizeof(int) * ncpu);
    int *buf = xmalloc(sizeof(int) * NP);
    int prox = 0, terminados = 0, t = 0;
    long faults = 0, sem_vitima = 0, referencias = 0;

    GUARDA_EV = log != NULL || a->ao_vivo;
    ao_vivo_ligado = a->ao_vivo;

    for (;;) {
        NEV = 0;

        /* 1) fim da unidade [t-1, t): fim de burst, termino ou fim de quantum */
        int nexp = 0;
        for (int k = 0; k < ncpu; k++) {
            CPU *c = &cpu[k];
            if (c->troca_fim == t) c->troca_fim = -1;
            if (!c->executou) continue;
            c->executou = 0;
            int i = c->proc;
            Proc *p = &P[i];
            p->pos++;
            c->quantum_usado++;
            if (p->pos == p->b[p->etapa].cpu) {        /* fim do burst de CPU */
                p->etapa++;
                p->pos = 0;
                c->proc = -1;
                if (p->etapa == p->nb) {
                    p->fim = t;
                    terminados++;
                    mem_libera_proc(&mem, i);
                    evento("TERMINO P%d (cpu%d)", p->pid, k);
                } else {
                    int es = p->b[p->etapa].es;
                    p->etapa++;
                    bloq_poe(&bloq, i, t + es, MOT_ES);
                    evento("FIM_BURST P%d (cpu%d) -> E/S ate %d", p->pid, k, t + es);
                }
            } else if (a->sched == RR && c->quantum_usado >= a->quantum) {
                expirados[nexp++] = k;
            }
        }
        if (terminados == NP) break;

        /* 2) chegadas e retornos de E/S / page fault, em ordem de pid */
        int nent = 0;
        while (prox < NP && P[prox].chegada == t) { ent_mot[nent] = -1; entrando[nent++] = prox++; }
        while (bloq.n && bloq.v[0].ret == t) {
            ItemBloq b = bloq_tira(&bloq);
            if (b.motivo == MOT_RETRY) {   /* tenta de novo conseguir um quadro */
                Proc *p = &P[b.proc];
                if (mem_carrega(&mem, b.proc, p->b[p->etapa].refs[p->pos])) {
                    bloq_poe(&bloq, b.proc, t + a->custo_fault, MOT_FAULT);
                    evento("QUADRO_OBTIDO P%d, carregando ate %d", p->pid, t + a->custo_fault);
                } else {
                    bloq_poe(&bloq, b.proc, t + a->custo_fault, MOT_RETRY);
                    evento("SEM_VITIMA P%d de novo, ate %d", p->pid, t + a->custo_fault);
                }
                continue;
            }
            ent_mot[nent] = b.motivo;
            entrando[nent++] = b.proc;
        }
        /* ordena por pid (indices -> pids) */
        for (int x = 1; x < nent; x++) {
            int v = entrando[x], mv = ent_mot[x], y = x - 1;
            while (y >= 0 && P[entrando[y]].pid > P[v].pid) {
                entrando[y + 1] = entrando[y];
                ent_mot[y + 1] = ent_mot[y];
                y--;
            }
            entrando[y + 1] = v;
            ent_mot[y + 1] = mv;
        }
        for (int x = 0; x < nent; x++) {
            Proc *p = &P[entrando[x]];
            const char *nome = ent_mot[x] < 0 ? "CHEGADA"
                             : ent_mot[x] == MOT_ES ? "RETORNO_ES" : "RETORNO_FAULT";
            fila_entra(&fila, entrando[x], t);
            evento("%s P%d", nome, p->pid);
        }

        /* 3) preempcao por quantum: vai para o fim da fila se ha outro pronto */
        for (int x = 0; x < nexp; x++) {
            CPU *c = &cpu[expirados[x]];
            if (fila.n) {
                int i = c->proc;
                c->proc = -1;
                fila_entra(&fila, i, t);
                evento("PREEMPCAO P%d (cpu%d)", P[i].pid, expirados[x]);
            } else {
                c->quantum_usado = 0;      /* continua sem custo, novo quantum */
            }
        }

        /* 4) despachos + referencia a pagina no inicio da unidade */
        int mudou = 1;
        while (mudou) {
            mudou = 0;
            for (int k = 0; k < ncpu; k++) {
                CPU *c = &cpu[k];
                if (c->proc < 0 && fila.n) {
                    int i = fila_sai(&fila);
                    Proc *p = &P[i];
                    p->espera += t - p->entrou_fila;
                    if (p->primeiro_despacho < 0) p->primeiro_despacho = t;
                    c->proc = i;
                    c->quantum_usado = 0;
                    if (p->pid != c->ultimo) {
                        c->n_trocas++;
                        if (a->custo_troca > 0) c->troca_fim = t + a->custo_troca;
                    }
                    c->ultimo = p->pid;
                    evento("DESPACHO P%d (cpu%d)%s", p->pid, k, c->troca_fim >= 0 ? " com troca" : "");
                }
                if (c->proc < 0 || c->troca_fim >= 0 || c->executou) continue;
                int i = c->proc;
                Proc *p = &P[i];
                int *refs = p->b[p->etapa].refs;
                if (!refs) { c->executou = 1; continue; }
                int pag = refs[p->pos];
                if (mem_acesso(&mem, i, pag)) {        /* acerto: executa [t, t+1) */
                    c->executou = 1;
                    referencias++;
                    continue;
                }
                /* page fault: bloqueia, CPU fica livre, resto do quantum e perdido */
                faults++;
                c->proc = -1;
                if (mem_carrega(&mem, i, pag)) {
                    bloq_poe(&bloq, i, t + a->custo_fault, MOT_FAULT);
                    evento("FAULT P%d pag %d (cpu%d) ate %d", p->pid, pag, k, t + a->custo_fault);
                } else {
                    sem_vitima++;
                    bloq_poe(&bloq, i, t + a->custo_fault, MOT_RETRY);
                    evento("FAULT P%d pag %d sem vitima, ate %d", p->pid, pag, t + a->custo_fault);
                }
                mudou = 1;
            }
        }

        /* 5) contabiliza a unidade [t, t+1) de cada CPU */
        for (int k = 0; k < ncpu; k++) {
            CPU *c = &cpu[k];
            int est;    /* -1 ociosa, -2 troca, 0 executando */
            if (c->proc < 0) { c->ocioso++; est = -1; }
            else if (c->troca_fim >= 0) { c->troca++; est = -2; }
            else { c->util++; est = 0; }
            if (t < JANELA_GANTT) {
                c->gantt_est[t] = est;
                c->gantt_pid[t] = c->proc >= 0 ? P[c->proc].pid : SEM_PID;
            }
        }

        if (log && NEV && (!a->log_ate || t <= a->log_ate))
            escreve_log(log, t, cpu, ncpu, &fila, &bloq, &mem, buf);

        if (ao_vivo_ligado) {
            painel(t, cpu, ncpu, &fila, &bloq, &mem, terminados, faults, buf);
            struct timespec ts = { a->atraso / 1000, (a->atraso % 1000) * 1000000L };
            nanosleep(&ts, NULL);
        }
        if (saiu_ao_vivo) {
            saiu_ao_vivo = 0;
            printf("\nmodo ao vivo encerrado em t=%d, terminando a simulacao...\n", t);
        }
        t++;
    }

    free(entrando); free(ent_mot); free(expirados); free(buf);
    free(fila.circ); free(fila.heap); free(bloq.v); free(mem.q);
    Resultado r = { cpu, t, faults, sem_vitima, referencias };
    return r;
}

/* ------------------------------------------------------------------------- */
/* Saidas                                                                     */
/* ------------------------------------------------------------------------- */
typedef struct {
    double espera_media, turnaround_medio, resposta_media, taxa_faults, util_media;
    long espera_max, trocas;
    int tem_classe[2];
    double espera_media_cl[2], turnaround_cl[2];
    long espera_max_cl[2];
} Metricas;

static const char *CLASSE[] = { "curto", "longo" };

static Metricas calcula(Resultado *r, int ncpu) {
    Metricas m = { 0 };
    double se = 0, st = 0, sr = 0;
    long n_cl[2] = { 0 };
    double se_cl[2] = { 0 }, st_cl[2] = { 0 };
    for (int i = 0; i < NP; i++) {
        Proc *p = &P[i];
        se += p->espera;
        st += p->fim - p->chegada;
        sr += p->primeiro_despacho - p->chegada;
        if (p->espera > m.espera_max) m.espera_max = p->espera;
        for (int c = 0; c < 2; c++)
            if (!strcmp(p->classe, CLASSE[c])) {
                n_cl[c]++;
                se_cl[c] += p->espera;
                st_cl[c] += p->fim - p->chegada;
                if (p->espera > m.espera_max_cl[c]) m.espera_max_cl[c] = p->espera;
            }
    }
    m.espera_media = se / NP;
    m.turnaround_medio = st / NP;
    m.resposta_media = sr / NP;
    long ocupado = 0;
    for (int k = 0; k < ncpu; k++) {
        m.trocas += r->cpu[k].n_trocas;
        ocupado += r->cpu[k].util + r->cpu[k].troca;
    }
    m.util_media = (double)ocupado / ((double)r->makespan * ncpu);
    m.taxa_faults = r->referencias ? (double)r->faults / (r->faults + r->referencias) : 0;
    for (int c = 0; c < 2; c++)
        if (n_cl[c]) {
            m.tem_classe[c] = 1;
            m.espera_media_cl[c] = se_cl[c] / n_cl[c];
            m.turnaround_cl[c] = st_cl[c] / n_cl[c];
        }
    return m;
}

static void resumo(FILE *o, Params *a, Resultado *r, Metricas *m) {
    int mk = r->makespan;
    char q[16];
    snprintf(q, sizeof q, "%d", a->quantum);
    const char *linha = "================================================================";
    fprintf(o, "%s\nRESUMO DA SIMULACAO\n%s\nParametros:\n", linha, linha);
    fprintf(o, "  %-13s %s\n  %-13s %d\n  %-13s %d\n  %-13s %s\n  %-13s %s\n"
               "  %-13s %s\n  %-13s %d\n  %-13s %d\n  %-13s %d\n",
            "arquivo", a->arquivo, "processos", NP, "cpus", a->cpus, "sched", NOME_SCHED[a->sched],
            "quantum", a->sched == RR ? q : "-", "subst", NOME_SUBST[a->subst], "quadros", a->quadros,
            "custo-troca", a->custo_troca, "custo-fault", a->custo_fault);
    fprintf(o, "\nMetricas globais:\n");
    fprintf(o, "  Makespan ............... %d\n", mk);
    fprintf(o, "  Espera media ........... %.2f\n", m->espera_media);
    fprintf(o, "  Espera maxima .......... %ld\n", m->espera_max);
    fprintf(o, "  Turnaround medio ....... %.2f\n", m->turnaround_medio);
    fprintf(o, "  Resposta media ......... %.2f\n", m->resposta_media);
    fprintf(o, "  Trocas de contexto ..... %ld\n", m->trocas);
    fprintf(o, "\nPor CPU:\n  CPU      util  %%util    troca %%troca   ocioso %%ocioso  trocas\n");
    for (int k = 0; k < a->cpus; k++) {
        CPU *c = &r->cpu[k];
        fprintf(o, "  %3d %9ld %5.1f%% %8ld %5.1f%% %8ld %6.1f%% %7ld\n", k, c->util,
                100.0 * c->util / mk, c->troca, 100.0 * c->troca / mk, c->ocioso,
                100.0 * c->ocioso / mk, c->n_trocas);
    }
    fprintf(o, "\nMemoria:\n");
    fprintf(o, "  Page faults ............ %ld\n", r->faults);
    fprintf(o, "  Referencias (acertos) .. %ld\n", r->referencias);
    fprintf(o, "  Taxa de faults ......... %.2f%%  (faults / acessos)\n", 100 * m->taxa_faults);
    fprintf(o, "  Faults sem vitima ...... %ld\n", r->faults_sem_vitima);
    if (m->tem_classe[0] && m->tem_classe[1]) {
        fprintf(o, "\nPor classe (so para analise; o escalonador nao usa):\n");
        for (int c = 0; c < 2; c++)
            fprintf(o, "  %s: espera media %.2f  max %ld  turnaround %.2f\n", CLASSE[c],
                    m->espera_media_cl[c], m->espera_max_cl[c], m->turnaround_cl[c]);
    }
    fprintf(o, "%s\n", linha);
}

/* metricas em chave=valor, lidas pelo experimentos.py e pelo testes.py */
static void grava_metricas(FILE *o, Resultado *r, Metricas *m, int ncpu) {
    fprintf(o, "makespan=%d\nespera_media=%.6f\nespera_max=%ld\nturnaround_medio=%.6f\n"
               "resposta_media=%.6f\ntrocas=%ld\nfaults=%ld\nfaults_sem_vitima=%ld\n"
               "referencias=%ld\ntaxa_faults=%.6f\nutil_media=%.6f\n",
            r->makespan, m->espera_media, m->espera_max, m->turnaround_medio, m->resposta_media,
            m->trocas, r->faults, r->faults_sem_vitima, r->referencias, m->taxa_faults, m->util_media);
    for (int c = 0; c < 2; c++)
        if (m->tem_classe[c])
            fprintf(o, "espera_media_%s=%.6f\nespera_max_%s=%ld\nturnaround_medio_%s=%.6f\n",
                    CLASSE[c], m->espera_media_cl[c], CLASSE[c], m->espera_max_cl[c], CLASSE[c],
                    m->turnaround_cl[c]);
    for (int k = 0; k < ncpu; k++)
        fprintf(o, "cpu%d=%ld,%ld,%ld,%ld\n", k, r->cpu[k].util, r->cpu[k].troca,
                r->cpu[k].ocioso, r->cpu[k].n_trocas);
}

static void grava_gantt(FILE *o, Resultado *r, int ncpu) {
    fprintf(o, "cpu,inicio,fim,evento\n");
    for (int k = 0; k < ncpu; k++) {
        CPU *c = &r->cpu[k];
        int n = r->makespan < JANELA_GANTT ? r->makespan : JANELA_GANTT;
        int ini = 0;
        for (int x = 1; x <= n; x++) {
            /* (estado, pid): trocas seguidas para processos diferentes ficam separadas */
            if (x == n || c->gantt_est[x] != c->gantt_est[ini] || c->gantt_pid[x] != c->gantt_pid[ini]) {
                if (c->gantt_est[ini] == -1) fprintf(o, "%d,%d,%d,OCIOSA\n", k, ini, x);
                else if (c->gantt_est[ini] == -2) fprintf(o, "%d,%d,%d,TROCA\n", k, ini, x);
                else fprintf(o, "%d,%d,%d,P%d\n", k, ini, x, c->gantt_pid[ini]);
                ini = x;
            }
        }
    }
}

static void grava_processos(FILE *o) {
    fprintf(o, "pid,classe,chegada,fim,espera,resposta,turnaround\n");
    for (int i = 0; i < NP; i++) {
        Proc *p = &P[i];
        fprintf(o, "%d,%s,%d,%d,%ld,%d,%d\n", p->pid, p->classe, p->chegada, p->fim, p->espera,
                p->primeiro_despacho - p->chegada, p->fim - p->chegada);
    }
}

static FILE *abre(const char *base, const char *suf) {
    char nome[1200];
    snprintf(nome, sizeof nome, "%s%s", base, suf);
    FILE *f = fopen(nome, "w");
    if (!f) morre("nao criou %s", nome);
    return f;
}

/* ------------------------------------------------------------------------- */
/* Argumentos e menu                                                          */
/* ------------------------------------------------------------------------- */
static int escolha(const char *v, const char **op, int n, const char *nome) {
    for (int k = 0; k < n; k++)
        if (!strcmp(v, op[k])) return k;
    morre("--%s invalido: %s", nome, v);
    return -1;
}

static void le_linha(const char *texto, const char *padrao, char *out, size_t tam) {
    printf("%s [%s]: ", texto, padrao);
    fflush(stdout);
    if (!fgets(out, tam, stdin)) out[0] = 0;
    out[strcspn(out, "\r\n")] = 0;
    if (!out[0]) snprintf(out, tam, "%s", padrao);
}

static int pergunta_int(const char *texto, int padrao) {
    char b[64], p[16];
    snprintf(p, sizeof p, "%d", padrao);
    for (;;) {
        le_linha(texto, p, b, sizeof b);
        char *fim;
        long v = strtol(b, &fim, 10);
        if (*fim == 0) return (int)v;
        printf("  valor invalido\n");
    }
}

static int pergunta_op(const char *texto, const char *padrao, const char **op, int n) {
    char b[64];
    for (;;) {
        le_linha(texto, padrao, b, sizeof b);
        for (int k = 0; k < n; k++)
            if (!strcmp(b, op[k])) return k;
        printf("  opcoes:");
        for (int k = 0; k < n; k++) printf(" %s", op[k]);
        printf("\n");
    }
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void menu(Params *a) {
    static const char *SN[] = { "n", "s" };
    printf("=== SIMULADOR DE ESCALONAMENTO + PAGINACAO ===\n");
    char *jsons[64];
    int nj = 0;
    DIR *d = opendir("cenarios");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && nj < 64) {
            size_t l = strlen(e->d_name);
            if (l > 5 && !strcmp(e->d_name + l - 5, ".json")) jsons[nj++] = strdup(e->d_name);
        }
        closedir(d);
        qsort(jsons, nj, sizeof(char *), cmp_str);
    }
    for (int k = 0; k < nj; k++) printf("  %d) %s\n", k + 1, jsons[k]);
    char b[512];
    le_linha("Arquivo (numero ou caminho)", nj ? "1" : "", b, sizeof b);
    int idx = atoi(b);
    if (idx >= 1 && idx <= nj) snprintf(a->arquivo, sizeof a->arquivo, "cenarios/%s", jsons[idx - 1]);
    else snprintf(a->arquivo, sizeof a->arquivo, "%s", b);
    for (int k = 0; k < nj; k++) free(jsons[k]);

    do a->cpus = pergunta_int("CPUs (1 ou 4)", 1); while (a->cpus != 1 && a->cpus != 4);
    a->sched = pergunta_op("Escalonador (fcfs/sjf/rr)", "fcfs", NOME_SCHED, 3);
    if (a->sched == RR) a->quantum = pergunta_int("Quantum", 10);
    a->subst = pergunta_op("Substituicao (fifo/lru)", "lru", NOME_SUBST, 2);
    a->quadros = pergunta_int("Quadros", 256);
    a->custo_troca = pergunta_int("Custo da troca", 1);
    a->custo_fault = pergunta_int("Custo do page fault", 20);
    a->log = pergunta_op("Gerar log? (s/n)", "n", SN, 2);
    if (a->log) a->log_ate = pergunta_int("Log ate o instante (0 = tudo)", 2000);
    a->ao_vivo = pergunta_op("Ver ao vivo? (s/n)", "n", SN, 2);
    if (a->ao_vivo) a->atraso = pergunta_int("Atraso por unidade em ms", 150);
}

static void uso(void) {
    printf("uso: ./simulador --arquivo ARQ.json [--cpus 1|4] [--sched fcfs|sjf|rr] [--quantum N]\n"
           "                 [--subst fifo|lru] [--quadros N] [--custo-troca N] [--custo-fault N]\n"
           "                 [--log] [--log-ate T] [--ao-vivo] [--atraso MS] [--saida DIR]\n"
           "       ./simulador          (menu interativo)\n");
}

static void args(int argc, char **argv, Params *a) {
    static struct option op[] = {
        { "arquivo", 1, 0, 'a' }, { "cpus", 1, 0, 'c' }, { "sched", 1, 0, 's' },
        { "quantum", 1, 0, 'q' }, { "subst", 1, 0, 'u' }, { "quadros", 1, 0, 'f' },
        { "custo-troca", 1, 0, 't' }, { "custo-fault", 1, 0, 'p' }, { "log", 0, 0, 'l' },
        { "log-ate", 1, 0, 'L' }, { "ao-vivo", 0, 0, 'v' }, { "atraso", 1, 0, 'd' },
        { "saida", 1, 0, 'o' }, { "help", 0, 0, 'h' }, { 0, 0, 0, 0 }
    };
    int o;
    while ((o = getopt_long(argc, argv, "", op, NULL)) != -1) {
        switch (o) {
        case 'a': snprintf(a->arquivo, sizeof a->arquivo, "%s", optarg); break;
        case 'c': a->cpus = atoi(optarg); break;
        case 's': a->sched = escolha(optarg, NOME_SCHED, 3, "sched"); break;
        case 'q': a->quantum = atoi(optarg); break;
        case 'u': a->subst = escolha(optarg, NOME_SUBST, 2, "subst"); break;
        case 'f': a->quadros = atoi(optarg); break;
        case 't': a->custo_troca = atoi(optarg); break;
        case 'p': a->custo_fault = atoi(optarg); break;
        case 'l': a->log = 1; break;
        case 'L': a->log_ate = atoi(optarg); break;
        case 'v': a->ao_vivo = 1; break;
        case 'd': a->atraso = atoi(optarg); break;
        case 'o': snprintf(a->saida, sizeof a->saida, "%s", optarg); break;
        case 'h': uso(); exit(0);
        default: uso(); exit(1);
        }
    }
    if (!a->arquivo[0]) { uso(); exit(1); }
}

int main(int argc, char **argv) {
    Params a = { .cpus = 1, .sched = FCFS, .quantum = 10, .subst = SUBST_LRU, .quadros = 256,
                 .custo_troca = 1, .custo_fault = 20, .log_ate = 2000, .atraso = 150 };
    snprintf(a.saida, sizeof a.saida, "saida");
    if (argc == 1) menu(&a);
    else args(argc, argv, &a);

    if ((a.cpus != 1 && a.cpus != 4) || a.quantum < 1 || a.quadros < 1 || a.custo_troca < 0 ||
        a.custo_fault < 1 || a.atraso < 0)
        morre("parametros invalidos");

    carrega_json(a.arquivo);
    for (int i = 0; i < NP; i++) P[i].primeiro_despacho = -1;

    mkdir(a.saida, 0755);
    /* nome da execucao: <cenario>_cpus<N>_<sched>[_q<Q>]_<subst>_f<quadros> */
    const char *bn = strrchr(a.arquivo, '/');
    bn = bn ? bn + 1 : a.arquivo;
    char cen[512], q[16] = "", base[1100];
    snprintf(cen, sizeof cen, "%s", bn);
    char *ponto = strrchr(cen, '.');
    if (ponto) *ponto = 0;
    if (a.sched == RR) snprintf(q, sizeof q, "_q%d", a.quantum);
    snprintf(base, sizeof base, "%s/%s_cpus%d_%s%s_%s_f%d", a.saida, cen, a.cpus,
             NOME_SCHED[a.sched], q, NOME_SUBST[a.subst], a.quadros);

    FILE *log = a.log ? abre(base, "_log.txt") : NULL;
    signal(SIGINT, trata_sigint);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    Resultado r = simular(&a, log);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (log) fclose(log);

    Metricas m = calcula(&r, a.cpus);
    resumo(stdout, &a, &r, &m);
    printf("tempo de execucao do simulador: %.2f s\n",
           (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);

    FILE *f = abre(base, "_resumo.txt");   resumo(f, &a, &r, &m);            fclose(f);
    f = abre(base, "_gantt.csv");          grava_gantt(f, &r, a.cpus);       fclose(f);
    f = abre(base, "_processos.csv");      grava_processos(f);               fclose(f);
    f = abre(base, "_metricas.txt");       grava_metricas(f, &r, &m, a.cpus); fclose(f);
    printf("arquivos: %s_{resumo.txt,gantt.csv,processos.csv,metricas.txt%s}\n", base,
           a.log ? ",log.txt" : "");
    return 0;
}
