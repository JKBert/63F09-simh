# Notes de lecture SIMH pour le simulateur 63F09

Lecture faite le 2026-11-10, avant le nettoyage de l'arbre, sur `simh/simh` master (698dc8a1). Elle sert de référence
au plan de `CLAUDE.md`. Les numéros de ligne valent pour ce commit.

## 1. Squelette SCP d'un simulateur (modèle : `swtp6800/`)

- Fichier système (`swtp6800/swtp6800/mp-a2_sys.c`, 91 lignes) : `sim_name`, `sim_PC` (pointeur vers le descripteur du PC),
  `sim_emax` (mots lus par `examine`), `sim_devices[]` (terminé par `NULL`), `sim_stop_messages[]` ; le chargeur
  `sim_load()` (`m6800.c:1985`) et le désassembleur `fprint_sym()` (`m6800.c:2104`) sont dans le fichier du CPU.
- CPU (`swtp6800/common/m6800.c`, 2 255 lignes) : registres globaux `int32`, `REG m6800_reg[]` (`HRDATA`...), un `UNIT`, un
  `DEVICE`, un `MTAB` (dont `HISTORY=n`), un `DEBTAB`.
- Boucle `sim_instr()` (`m6800.c:332`) : `while (reason == 0)` ; `if (sim_interval <= 0) sim_process_event()` ; test des
  interruptions à chaque instruction ; point d'arrêt testé seulement si `sim_brk_summ` ; historique des instructions
  (tampon circulaire `hst`) ; `sim_interval--` **par instruction** (pour nous : retrancher les cycles) ; un grand `switch (IR)`.
- Accès mémoire : `fetch_byte()` passe par `CPU_BD_get_mbyte()` (appel indirect par octet, trop lent pour 43 MIPS : prendre
  le modèle du VAX, voir §3).
- Série (`swtp6800/common/mp-s.c`) : console sondée par `sim_poll_kbd()`/`sim_putchar()` depuis un service `sim_activate`.

## 2. Reprise après faute (modèle : `VAX/vax_cpu.c`)

- `ABORT(x)` = `longjmp(save_env, x)` (`vax_defs.h:79`) ; `setjmp` une seule fois à l'entrée de `sim_instr` (`vax_cpu.c:~518`).
- **File de récupération `recq[6]`** (`vax_cpu.c:255`, macros `RQ_REC/RQ_GETRN/RQ_DIR`, `vax_defs.h:125-131`) : chaque
  auto-incrémentation/décrémentation modifie le registre **tout de suite** et ajoute (registre, longueur, sens) à la file
  (`vax_cpu.c:885-890`). Sur `abortval < 0` (`vax_cpu.c:~531`), le gestionnaire **défait** les registres dans l'ordre, restaure
  le PC à `fault_PC` (début de l'instruction), vide la file puis prend l'exception (`intexc`).
- Pour le 63F09 : `,R+`, `,R++`, `,-R`, `,--R` se traitent comme le VAX (largeur de la donnée en mode 32 bits) ; `PSHS`/`PULS`/
  `PSHU`/`PULU` et `TFM` modifient plusieurs registres : instantané de `S`/`U`/`X`/`Y`/`W` au début de l'instruction
  concernée. Le matériel restaure **tous** les registres internes à une faute de page (`63F29.vhd` ~l.451) ; `TFM` reprend à
  l'itération en cours (registres déjà avancés, PC sur l'opcode). À mesurer : la file coûte une écriture par auto-incrément.

## 3. MMU et accès mémoire (modèle : `VAX/vax_mmu.h`, `vax_mmu.c`)

- Chemin rapide en `static SIM_INLINE` dans l'**en-tête** (`vax_mmu.h:100-290`) : `Read(va, lnt, acc)`/`Write(...)` consultent un
  TLB à correspondance directe `TLBENT {tag, pte}` (`stlb[]`/`ptlb[]`) ; **un seul `AND`** du masque d'accès contre les bits
  du `pte` vérifie les droits ; sur échec, `fill()` (`vax_mmu.c:141`) traite **toutes** les erreurs (appelle `ABORT`).
- Mémoire physique : `ADDR_IS_MEM(pa)` -> tableau `M[pa >> 2]` ; sinon `ADDR_IS_IO` -> `ReadIO()`/`WriteIO()`, sinon
  registres. Les accès non alignés ou à cheval sur deux pages sont traités en deux phases (traduire les deux pages d'abord).
- Différence avec le 63F09 : la MMU du 63F09 n'a **pas de table de pages parcourue par le matériel** ; ses 64 indirections par
  anneau forment un tableau associatif géré par logiciel (comparaison de 20 bits d'étiquette, « premier qui correspond gagne »,
  doublon = faute), avec vieillissement LRU. Le `fill()` du 63F09 est donc une recherche dans ≤ 64 entrées de l'anneau courant,
  et le chemin rapide doit **mettre à jour l'âge** à chaque accès traduit si l'on veut le LRU exact (sinon : ordre de
  remplissage seulement). Un tableau de micro-TLB par anneau (4) évite de tester l'anneau à chaque accès.
- Accès à plusieurs octets : le bus du 63F09 est sur 8 bits ; les données de 16/32/64 bits sont lues octet par octet, donc
  traduire chaque page touchée.

## 4. Affichage graphique (modèle : `VAX/vax_vc.c`, 1 115 lignes, QVSS monochrome)

- Création au `reset` (`vax_vc.c:1010-1062`) : `vid_open(dptr, NULL, XSIZE, YSIZE, flags)`, tampon d'image 32 bits
  (`vc_lines`), tampon vidéo invité (`vc_buf`), **carte des lignes sales** (`vc_map[]`, bit `VCMAP_VLD` = ligne à jour),
  palette par `vid_map_rgb`.
- Service périodique `vc_svc` (`vax_vc.c:870-985`) coplanifié à `tmxr_poll` (`sim_clock_coschedule`) : curseur
  (`vid_set_cursor`, `vid_set_cursor_position`), **clavier/souris** par `vid_poll_kb()`/`vid_poll_mouse()` poussés vers le
  matériel émulé, conversion des **seules lignes invalidées** puis `vid_draw(x, y, w, h, buf)` par blocs contigus et
  `vid_refresh()` ; interruption de synchro verticale.
- Pour le compagnon FPGA : mémoire vidéo visible de l'invité, un crochet d'écriture qui invalide les lignes touchées, un
  service à ~60 Hz ; mode « entrée capturée » de `sim_video` pour la souris.

## 5. Réseau (modèle : `PDP11/pdp11_xq.c`, API `sim_ether.h`)

- `attach` (`xq_attach`) ouvre l'interface (`eth_open(dev, name, dptr, dbg)`) ; le nom peut être une interface PCAP/TAP/VDE ou
  `nat:` (slirp, sans droits root). Réception : un service `xq_svc` lit **tous** les paquets disponibles (`eth_read`), les met
  en file, puis `xq_process_rbdl` les range dans les descripteurs de l'invité ; `eth_set_async(dev, latence)` rend la lecture
  asynchrone. Émission : `eth_write(dev, &paquet, rappel)` non bloquant. Filtre d'adresses : `eth_filter`/`eth_filter_hash_ex`
  (MAC, multicast, mode promiscuité). `eth_show` liste les interfaces.

## 6. Console série / ACIA (modèles : `AltairZ80/altairz80_sio.c`, `swtp6800/common/mp-s.c`)

- `altairz80_sio.c` simule une **ACIA de type 6850/2SIO** avec des lignes `TMLN`/`TMXR` (`sim_tmxr`) : le plus proche du
  `63F50`, avec `attach` pour le telnet. `mp-s.c` montre la voie minimale (`sim_poll_kbd`/`sim_putchar`).

## 7. Temps

- `sim_activate(unit, délai)` : délai en unités de `sim_interval` (nous : cycles du cœur) ; `sim_clock_coschedule(unit, délai)` :
  alignement sur l'horloge temps réel/`tmxr_poll` (périphériques lents : console, vidéo, réseau). `sim_rtcn_init/calb` : horloge
  d'interruption calibrée sur le temps hôte.

## 8. Décisions qui en découlent

1. Mémoire physique : tableau + `ADDR_IS_MEM` en ligne, E/S par table de fonctions (pas d'appel indirect par octet).
2. Registres validés après l'accès mémoire **ou** `recq` à la VAX ; `longjmp` pour les fautes ; `setjmp` une fois par `sim_instr`.
3. Boucle : un seul indicateur d'événement en attente (interruptions, `sim_interval`, points d'arrêt) pour sortir du chemin
   chaud ; historique optionnel.
4. Le temps avance par cycles (`cycles_63F09`), pas par instruction.
5. ACIA sur `TMXR` (modèle Altair 2SIO), compagnon vidéo sur le modèle `vax_vc.c`, réseau sur le modèle `pdp11_xq.c`.
