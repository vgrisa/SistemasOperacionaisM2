# Makefile - simulador M2
#   make          compila o simulador (C + biblioteca cJSON em lib/)
#   make teste    confere com os exemplos do enunciado
#   make limpar   apaga o executavel e a pasta saida/

CC     = gcc
CFLAGS = -O2 -Wall -Wextra -std=c11

simulador: simulador.c lib/cJSON.c lib/cJSON.h
	$(CC) $(CFLAGS) simulador.c lib/cJSON.c -o simulador

teste: simulador
	python3 testes.py

limpar:
	rm -rf simulador saida
