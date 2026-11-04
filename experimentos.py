#!/usr/bin/env python3
"""Roda o ./simulador (C) em todas as execuções da seção 11 e gera tabelas, gráficos e Gantt em resultados/.

    pip install matplotlib
    make && python3 experimentos.py

Execuções:
  base         2 cenários x {1, 4} CPUs x {FCFS, SJF, RR q=2,5,10,20,50}, LRU, 256 quadros = 28
  fifo x lru   as 8 configurações FCFS e RR q=10 (A/B x 1/4 CPUs) repetidas com FIFO
  quadros      RR q=10 com 4 CPUs, LRU e FIFO, 64..1024 quadros (opcional do enunciado)
"""
import csv
import glob
import os
import subprocess
import sys
import tempfile
from multiprocessing import Pool

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

AQUI = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(AQUI, "simulador")
CEN = {"A": "cenarios/cenario_A_t0.json", "B": "cenarios/cenario_B_espacadas.json"}
OUT = os.path.join(AQUI, "resultados")
QUANTUMS = [2, 5, 10, 20, 50]
VARIACOES = [("fcfs", 0), ("sjf", 0)] + [("rr", q) for q in QUANTUMS]


def rotulo(sched, q):
    return "RR q=%d" % q if sched == "rr" else sched.upper()


def numero(v):
    if "," in v:
        return tuple(int(x) for x in v.split(","))
    return float(v) if "." in v else int(v)


def executa(cfg):
    """Roda ./simulador com a configuração e lê as métricas e o Gantt que ele grava."""
    cen, cpus, sched, q, subst, quadros = cfg
    with tempfile.TemporaryDirectory() as d:
        subprocess.run([BIN, "--arquivo", os.path.join(AQUI, CEN[cen]), "--cpus", str(cpus),
                        "--sched", sched, "--quantum", str(q or 10), "--subst", subst,
                        "--quadros", str(quadros), "--saida", d],
                       check=True, stdout=subprocess.DEVNULL)
        with open(glob.glob(os.path.join(d, "*_metricas.txt"))[0]) as f:
            m = {k: numero(v) for k, v in (l.strip().split("=", 1) for l in f)}
        with open(glob.glob(os.path.join(d, "*_gantt.csv"))[0]) as f:
            gantt = f.read()
    m.update(cenario=cen, cpus=cpus, sched=sched, quantum=q, subst=subst, quadros=quadros,
             config=rotulo(sched, q))
    return cfg, m, gantt


def desenha_gantt(gantt, ncpu, titulo, arq):
    cores = plt.get_cmap("tab20")
    fig, ax = plt.subplots(figsize=(18, 1.2 + 0.9 * ncpu))
    for linha in gantt.splitlines()[1:]:
        i, ini, fim, ev = linha.split(",")
        i, ini, fim = int(i), int(ini), int(fim)
        y = ncpu - 1 - i
        if ev == "OCIOSA":
            cor, txt = "white", ""
        elif ev == "TROCA":
            cor, txt = "#444444", ""
        else:
            cor, txt = cores(int(ev[1:]) % 20), ev[1:]
        ax.barh(y, fim - ini, left=ini, height=0.8, color=cor, edgecolor="black", linewidth=0.4,
                hatch="//" if ev == "OCIOSA" else None)
        if txt and fim - ini >= 2:
            ax.text((ini + fim) / 2, y, txt, ha="center", va="center", fontsize=7)
    ax.set_yticks(range(ncpu))
    ax.set_yticklabels(["CPU %d" % i for i in reversed(range(ncpu))])
    ax.set_xlim(0, 200)
    ax.set_xticks(range(0, 201, 10))
    ax.set_xlabel("tempo (numero = pid; cinza escuro = TROCA; hachurado = OCIOSA)")
    ax.set_title(titulo)
    fig.tight_layout()
    fig.savefig(arq, dpi=130)
    plt.close(fig)


def grava_csv(linhas, campos, arq):
    with open(arq, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=campos, extrasaction="ignore")
        w.writeheader()
        for m in linhas:
            w.writerow({k: (round(v, 4) if isinstance(v, float) else v) for k, v in m.items()})


def tabela_md(linhas, colunas):
    s = "| " + " | ".join(c[0] for c in colunas) + " |\n"
    s += "|" + "---|" * len(colunas) + "\n"
    for m in linhas:
        s += "| " + " | ".join(fmt % f(m) for _, f, fmt in colunas) + " |\n"
    return s


def main():
    if not os.path.exists(BIN):
        sys.exit("compile antes: make")
    os.makedirs(os.path.join(OUT, "gantt"), exist_ok=True)
    base = [(c, n, s, q, "lru", 256) for c in "AB" for n in (1, 4) for s, q in VARIACOES]
    fifo = [(c, n, s, q, "fifo", 256) for c in "AB" for n in (1, 4) for s, q in (("fcfs", 0), ("rr", 10))]
    quad = [(c, 4, "rr", 10, sub, f) for c in "AB" for sub in ("lru", "fifo")
            for f in (64, 128, 256, 512, 1024)]
    todas = list(dict.fromkeys(base + fifo + quad))

    with Pool() as pool:
        res = {cfg: (m, g) for cfg, m, g in pool.map(executa, todas)}
    M = {cfg: m for cfg, (m, _) in res.items()}

    # speedup / eficiência: makespan(1 CPU) / makespan(4 CPUs), mesma config
    for cfg, m in M.items():
        c, n, s, q, sub, f = cfg
        um, quatro = M.get((c, 1, s, q, sub, f)), M.get((c, 4, s, q, sub, f))
        if um and quatro:
            m["speedup"] = um["makespan"] / quatro["makespan"]
            m["eficiencia"] = m["speedup"] / 4

    # ---- Gantt das 28 execuções base -------------------------------------------------
    for cfg in base:
        c, n, s, q, sub, f = cfg
        nome = "%s_cpus%d_%s" % (c, n, rotulo(s, q).replace(" ", "").replace("=", ""))
        with open(os.path.join(OUT, "gantt", nome + ".csv"), "w") as fp:
            fp.write(res[cfg][1])
        desenha_gantt(res[cfg][1], n, "Cenario %s - %d CPU(s) - %s - LRU 256" % (c, n, rotulo(s, q)),
                      os.path.join(OUT, "gantt", nome + ".png"))

    # ---- tabelas ------------------------------------------------------------------------
    campos = ["cenario", "cpus", "config", "subst", "quadros", "espera_media", "espera_max",
              "turnaround_medio", "resposta_media", "makespan", "speedup", "eficiencia", "trocas",
              "util_media", "faults", "taxa_faults", "faults_sem_vitima",
              "espera_media_curto", "espera_media_longo", "espera_max_longo"]
    grava_csv([M[c] for c in todas], campos, os.path.join(OUT, "execucoes.csv"))

    cols = [("CPUs", lambda m: m["cpus"], "%d"), ("Escalonador", lambda m: m["config"], "%s"),
            ("Espera média", lambda m: m["espera_media"], "%.1f"),
            ("Espera máx", lambda m: m["espera_max"], "%d"),
            ("Turnaround", lambda m: m["turnaround_medio"], "%.1f"),
            ("Resposta", lambda m: m["resposta_media"], "%.1f"),
            ("Makespan", lambda m: m["makespan"], "%d"),
            ("Speedup", lambda m: m["speedup"], "%.2f"),
            ("Eficiência", lambda m: m["eficiencia"], "%.2f"),
            ("Trocas", lambda m: m["trocas"], "%d"),
            ("Util. CPU", lambda m: 100 * m["util_media"], "%.1f%%"),
            ("Faults", lambda m: m["faults"], "%d"),
            ("Taxa faults", lambda m: 100 * m["taxa_faults"], "%.1f%%"),
            ("Sem vítima", lambda m: m["faults_sem_vitima"], "%d")]
    md = []
    for c in "AB":
        md.append("## Cenário %s (LRU, 256 quadros)\n\n" % c)
        md.append(tabela_md([M[x] for x in base if x[0] == c], cols))
        md.append("\n### Por CPU\n\n| CPUs | Escalonador | CPU | Útil | Troca | Ocioso | %Útil | %Troca | %Ocioso | Trocas |\n|---|---|---|---|---|---|---|---|---|---|\n")
        for x in base:
            if x[0] != c:
                continue
            m = M[x]
            for i in range(x[1]):
                u, tr, oc, nt = m["cpu%d" % i]
                mk = m["makespan"]
                md.append("| %d | %s | %d | %d | %d | %d | %.1f%% | %.1f%% | %.1f%% | %d |\n" % (
                    x[1], m["config"], i, u, tr, oc, 100 * u / mk, 100 * tr / mk, 100 * oc / mk, nt))
        md.append("\n")
    md.append("## FIFO x LRU (256 quadros)\n\n| Cenário | CPUs | Escalonador | Faults LRU | Faults FIFO | Taxa LRU | Taxa FIFO | Makespan LRU | Makespan FIFO | Espera LRU | Espera FIFO |\n|---|---|---|---|---|---|---|---|---|---|---|\n")
    for x in fifo:
        f_, l_ = M[x], M[x[:4] + ("lru", 256)]
        md.append("| %s | %d | %s | %d | %d | %.1f%% | %.1f%% | %d | %d | %.1f | %.1f |\n" % (
            x[0], x[1], f_["config"], l_["faults"], f_["faults"], 100 * l_["taxa_faults"],
            100 * f_["taxa_faults"], l_["makespan"], f_["makespan"], l_["espera_media"], f_["espera_media"]))
    md.append("\n## Variação de quadros (4 CPUs, RR q=10)\n\n| Cenário | Subst | Quadros | Faults | Taxa | Sem vítima | Makespan | Espera média | Util. CPU |\n|---|---|---|---|---|---|---|---|---|\n")
    for x in quad:
        m = M[x]
        md.append("| %s | %s | %d | %d | %.1f%% | %d | %d | %.1f | %.1f%% |\n" % (
            x[0], x[4].upper(), x[5], m["faults"], 100 * m["taxa_faults"], m["faults_sem_vitima"],
            m["makespan"], m["espera_media"], 100 * m["util_media"]))
    with open(os.path.join(OUT, "tabelas.md"), "w") as f:
        f.write("# Resultados (gerado por experimentos.py)\n\n" + "".join(md))

    # ---- gráficos -----------------------------------------------------------------------
    nomes = [rotulo(s, q) for s, q in VARIACOES]
    x = range(len(VARIACOES))

    def g(c, n, s, q, k):
        return M[(c, n, s, q, "lru", 256)][k]

    # 1) espera média por escalonador
    fig, axs = plt.subplots(1, 2, figsize=(13, 4.5))
    for ax, c in zip(axs, "AB"):
        for n, w in ((1, -0.2), (4, 0.2)):
            ax.bar([i + w for i in x], [g(c, n, s, q, "espera_media") for s, q in VARIACOES],
                   0.4, label="%d CPU(s)" % n)
        ax.set_xticks(list(x)); ax.set_xticklabels(nomes, rotation=30)
        ax.set_title("Cenario %s - espera media" % c); ax.set_ylabel("unidades de tempo"); ax.legend()
    fig.tight_layout(); fig.savefig(os.path.join(OUT, "g1_espera_por_escalonador.png"), dpi=130); plt.close(fig)

    # 2) efeito do quantum no RR
    fig, axs = plt.subplots(1, 3, figsize=(15, 4.3))
    for ax, k, t in zip(axs, ("espera_media", "resposta_media", "trocas"),
                        ("espera media", "resposta media", "trocas de contexto")):
        for c in "AB":
            for n in (1, 4):
                ax.plot(QUANTUMS, [g(c, n, "rr", q, k) for q in QUANTUMS], marker="o",
                        label="%s, %d CPU(s)" % (c, n))
        ax.set_xscale("log"); ax.set_xticks(QUANTUMS); ax.set_xticklabels(QUANTUMS); ax.minorticks_off()
        ax.set_xlabel("quantum"); ax.set_title("RR - " + t); ax.legend(fontsize=8)
    fig.tight_layout(); fig.savefig(os.path.join(OUT, "g2_efeito_quantum.png"), dpi=130); plt.close(fig)

    # 3) speedup por escalonador
    fig, ax = plt.subplots(figsize=(9, 4.3))
    for c, w in (("A", -0.2), ("B", 0.2)):
        ax.bar([i + w for i in x], [g(c, 4, s, q, "speedup") for s, q in VARIACOES], 0.4,
               label="Cenario %s" % c)
    ax.axhline(4, color="gray", ls="--", lw=1); ax.text(len(x) - 0.5, 4.05, "ideal = 4", ha="right")
    ax.set_xticks(list(x)); ax.set_xticklabels(nomes, rotation=30)
    ax.set_ylabel("speedup (makespan 1 CPU / 4 CPUs)"); ax.set_ylim(0, 0.5 + max(g(c, 4, s, q, "speedup") for c in "AB" for s, q in VARIACOES)); ax.legend()
    ax.set_title("Speedup por escalonador (LRU, 256 quadros)")
    fig.tight_layout(); fig.savefig(os.path.join(OUT, "g3_speedup.png"), dpi=130); plt.close(fig)

    # 4) quadros x faults (FIFO e LRU)
    fig, axs = plt.subplots(1, 2, figsize=(12, 4.3))
    fs = (64, 128, 256, 512, 1024)
    for c in "AB":
        for sub in ("lru", "fifo"):
            axs[0].plot(fs, [M[(c, 4, "rr", 10, sub, f)]["faults"] for f in fs], marker="o",
                        label="%s %s" % (c, sub.upper()))
            axs[1].plot(fs, [M[(c, 4, "rr", 10, sub, f)]["makespan"] for f in fs], marker="o",
                        label="%s %s" % (c, sub.upper()))
    for ax, t in zip(axs, ("page faults", "makespan")):
        ax.set_xscale("log", base=2); ax.set_xticks(fs); ax.set_xticklabels(fs); ax.minorticks_off()
        ax.set_xlabel("quadros"); ax.set_title("4 CPUs, RR q=10 - " + t); ax.legend()
    fig.tight_layout(); fig.savefig(os.path.join(OUT, "g4_quadros.png"), dpi=130); plt.close(fig)

    # 5) espera por classe (curto x longo) - injustiça / starvation
    fig, axs = plt.subplots(1, 2, figsize=(13, 4.5))
    for ax, c in zip(axs, "AB"):
        for cl, w in (("curto", -0.2), ("longo", 0.2)):
            ax.bar([i + w for i in x], [g(c, 4, s, q, "espera_media_" + cl) for s, q in VARIACOES],
                   0.4, label=cl)
        ax.set_xticks(list(x)); ax.set_xticklabels(nomes, rotation=30)
        ax.set_title("Cenario %s, 4 CPUs - espera media por classe" % c); ax.legend()
    fig.tight_layout(); fig.savefig(os.path.join(OUT, "g5_espera_por_classe.png"), dpi=130); plt.close(fig)

    print("%d execucoes. Resultados em %s" % (len(todas), OUT))


if __name__ == "__main__":
    main()
