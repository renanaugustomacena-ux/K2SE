# tools/re — strumenti di reverse engineering statico (capstone, nessun debugger)

Tutti leggono `swkotor2.exe.pre-laa-backup` (o l'exe live) tramite `tools/peimage.py`.

| Script | Uso | Cosa fa |
|---|---|---|
| `strxref.py "hitdist" ...` | stringhe ASCII esatte (case-sensitive) | trova la stringa in .rdata/.data e ogni `imm32` in .text che la referenzia, con disassembly di contesto |
| `strxref_ci.py PERSPACE DriveMaxSpeed ...` | come sopra ma case-insensitive (`MAXCTX=n` per il numero di contesti) | i nomi colonna 2DA nell'exe hanno case misto: `PERSPACE`, `CREPERSPACE`, `DRIVEACCl`, `DriveMaxSpeed`, `WalkRate`... |
| `dwxref.py 0xA0FDE4 ...` | xref a una VA qualsiasi (globali, vtable, funzioni) | `CTX`, `BEFORE`, `AFTER` regolano il contesto |
| `dispscan.py 0x2ec --word` | sweep lineare di .text | ogni istruzione che tocca `[reg+disp]` (opz. solo operandi a 16 bit): trova lettori/scrittori di un campo struct |
| `funcdump.py 0x00865830 0x00867AB0 out.asm` | disassembla una funzione intera su file | stampa riepilogo: call target, costanti float (con valore), globali, offset `[reg+imm]` usati |

Esempio (sessione 2026-09-02): `python tools/re/strxref_ci.py PERSPACE CREPERSPACE` ha localizzato il loader degli indici colonna di `appearance.2da` (0x006E7E40..0x006E841B) e i globali indice (`0xA0FD9C` CREPERSPACE, `0xA0FDE4` PERSPACE, ...), da cui `dwxref.py` ha portato ai consumatori server (0x0057FD00) e client (0x0085Axxx).
