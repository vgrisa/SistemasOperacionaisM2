#!/usr/bin/env python3
"""Confere o simulador (./simulador, em C) contra os exemplos do enunciado (5.4, 5.5 e 7).

    make teste      (ou: make && python3 testes.py)
"""
import csv
import glob
import os
import subprocess
import tempfile

AQUI = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(AQUI, "simulador")


def roda(arq, **kw):
    """Roda o simulador e devolve (processos por pid, metricas)."""
    with tempfile.TemporaryDirectory() as d:
        cmd = [BIN, "--arquivo", os.path.join(AQUI, "exemplos", arq), "--saida", d]
        for k, v in kw.items():
            cmd += ["--" + k.replace("_", "-"), str(v)]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
        with open(glob.glob(os.path.join(d, "*_processos.csv"))[0]) as f:
            procs = sorted(csv.DictReader(f), key=lambda p: int(p["pid"]))
        with open(glob.glob(os.path.join(d, "*_metricas.txt"))[0]) as f:
            m = dict(linha.strip().split("=", 1) for linha in f)
    return procs, m


def col(procs, campo):
    return [int(p[campo]) for p in procs]


falhas = 0


def confere(nome, obtido, esperado):
    global falhas
    ok = obtido == esperado
    falhas += not ok
    print("%-4s %-45s obtido=%s esperado=%s" % ("OK" if ok else "FALHOU", nome, obtido, esperado))


# 5.4 - processo 17 sem page faults (bursts sem refs) e sem troca
p, m = roda("ex17_sem_memoria.json", custo_troca=0)
confere("5.4 turnaround P17", col(p, "turnaround"), [19])
confere("5.4 espera P17", col(p, "espera"), [0])

# 5.5 - processo 17 com page faults: 1 CPU, troca ignorada, fault = 20
p, m = roda("ex17.json", custo_troca=0, custo_fault=20)
confere("5.5 turnaround P17", col(p, "turnaround"), [99])
confere("5.5 termino P17", col(p, "fim"), [441])
confere("5.5 page faults", int(m["faults"]), 4)

# 7.1 - sem memória, 1 CPU, FCFS, troca = 1
p, m = roda("ex71.json", sched="fcfs", custo_troca=1)
confere("7.1 espera", col(p, "espera"), [0, 6, 8])
confere("7.1 resposta", col(p, "resposta"), [0, 6, 8])
confere("7.1 turnaround", col(p, "turnaround"), [6, 10, 11])
u, tr, oc, _ = map(int, m["cpu0"].split(","))
confere("7.1 util/troca/ocioso/makespan", (u, tr, oc, int(m["makespan"])), (10, 3, 0, 13))
confere("7.1 espera media", round(float(m["espera_media"]), 2), 4.67)
confere("7.1 turnaround medio", float(m["turnaround_medio"]), 9)

# 7.1 refeito com SJF e RR (q=2), calculado à mão:
#   SJF: troca 0-1, P2 1-4 | troca 4-5, P3 5-7 | troca 7-8, P1 8-13
p, m = roda("ex71.json", sched="sjf", custo_troca=1)
confere("7.1 SJF espera", col(p, "espera"), [7, 0, 2])
confere("7.1 SJF makespan", int(m["makespan"]), 13)
#   RR q=2: P1 1-3 | P2 4-6 | P3 7-9 (fim) | P1 10-12 | P2 13-14 (fim) | P1 15-16 (fim)
p, m = roda("ex71.json", sched="rr", quantum=2, custo_troca=1)
confere("7.1 RR q2 espera", col(p, "espera"), [8, 9, 4])
confere("7.1 RR q2 termino", col(p, "fim"), [16, 14, 9])
confere("7.1 RR q2 trocas", int(m["trocas"]), 6)

# 7.2 - page faults e fila: 1 CPU, FCFS, troca = 0, fault = 5
p, m = roda("ex72.json", sched="fcfs", custo_troca=0, custo_fault=5)
confere("7.2 espera", col(p, "espera"), [0, 0, 0])
confere("7.2 turnaround", col(p, "turnaround"), [12, 12, 6])
confere("7.2 makespan", int(m["makespan"]), 13)

# 7.3 - a fila aparece: 1 CPU, troca = 0, fault = 20
p, m = roda("ex73.json", sched="fcfs", custo_troca=0, custo_fault=20)
confere("7.3 FCFS espera", col(p, "espera"), [0, 4, 7])
confere("7.3 FCFS termino", col(p, "fim"), [24, 27, 29])
p, m = roda("ex73.json", sched="sjf", custo_troca=0, custo_fault=20)
confere("7.3 SJF espera", col(p, "espera"), [5, 2, 0])
confere("7.3 SJF termino", col(p, "fim"), [29, 25, 22])

print("\n%s" % ("todos os testes passaram" if not falhas else "%d teste(s) falharam" % falhas))
raise SystemExit(1 if falhas else 0)
