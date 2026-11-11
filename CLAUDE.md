# Contexte projet : simulateur SIMH du SoC 63F09

Fork de `simh/simh` (`git@github.com:JKBert/simh.git`, branche `master`, base 698dc8a1, 2026-10-25) destiné à recevoir un
simulateur fonctionnel rapide du SoC **63F09** (extension VHDL du 6809 conçue par l'utilisateur, modes 16 et 32 bits, FPU,
MMU à 4 anneaux, jusqu'à 8 cœurs). Rédigé le 2026-11-10. **Aucun code n'est écrit à ce jour** : ce document est le plan.

Le chantier est **en pause jusqu'à la fin du SoC** (AMC = cache associatif de niveau 2, DMA, interface I2S) : décision de
l'utilisateur. Ce plan sert à reprendre sans repartir de zéro.

## Pourquoi un simulateur fonctionnel

L'émulateur exact du SoC (`~/cvs/gcc/github/63F09-emulator`, `build/soc63f09`) est **généré du VHDL signal par signal**
(simulation du RTL compilée en C). Mesuré le 2026-11-10 sur un cœur isolé : 82,7 Mticks/s au repos, 43 Mticks/s en
exécution, soit environ **13 000 instructions/s**, **3 300 fois plus lent que le vrai SoC** (qui fait environ 43 MIPS sur une
boucle serrée d'instructions courtes ; 1 tick = 1/144 ns). Deux cœurs instanciés : 6 200 fois plus lent ; 8 cœurs actifs :
4,8 Mticks/s. Un démarrage de NetBSD (~1e9 instructions) prendrait plus de 20 h. Il n'y a pas de drapeau « rapide » à
ajouter : il faut **un nouveau modèle fonctionnel du jeu d'instructions**, l'émulateur RTL restant l'**oracle**.

Choix SIMH plutôt que QEMU (discuté avec l'utilisateur, 2026-11-10) : interpréteur C, donc `FUSE` exact (le compte de cycles se
retranche par instruction, l'ordre des accès dans `RTI`/`PULS` est du code ordinaire), vieillissement LRU de la MMU exact
possible (chaque accès passe par la traduction), défauts de page précis triviaux, mono-thread donc déterministe (courses
entre cœurs reproductibles), pas de fork de QEMU à rebaser. Coût : 50 à 150 MIPS au total (8 cœurs actifs se les partagent),
pas de virtio, pas de stub gdb natif. QEMU reste possible plus tard si l'on isole le noyau fonctionnel (voir architecture).

Objectif de vitesse : au moins **43 MIPS en monocœur**, soit la vitesse du matériel.

## Sources de vérité (par ordre de priorité)

1. **Le VHDL** : `~/vhdl/63F09/63F09.srcs/sources_1/imports/rtl/` — `63F09.vhd` (cœur, l'en-tête liste tous les opcodes avec
   un marqueur de disponibilité : rien = 6809/6309/63F09, `*` = 6309+63F09, `#` = 63F09 seul, `%` = 6309 seul ; ces marqueurs
   sont parfois imprécis, c'est le décodage réel qui fait foi), `63F29.vhd` (MMU), `63F39.vhd` (FPU), `63F81.vhd` (BAC,
   arbitre de bus, cœurs), `63F21` PIA, `63F40` PTM, `63F50` ACIA, `63F52` SPI, `63F54` TWI, `63F46` watchdog, `63F75`
   horloge, `SoC.vhd` (carte mémoire). Le cache DMC (`63F41`) n'est **pas** modélisé (transparent pour le logiciel).
2. **L'émulateur RTL** `~/cvs/gcc/github/63F09-emulator` : oracle exécutable (`build/soc63f09 --elf f.elf --ticks N --dump F`),
   `docs/MEMORY_MAP.md` et `docs/EMULATOR.md`. Quand le modèle et l'émulateur divergent, l'émulateur tranche, puis le
   datasheet (Atkinson, *Motorola 6809 and Hitachi 6309 Programming Reference*, `~/datasheets/CPU/`).
3. **L'outillage** `~/cvs/gcc/binutils-gdb` : `CLAUDE.md` (historique de ~67 révisions, tous les pièges), `opcodes/m6809-opc.c`
   (table des ~1 400 lignes d'instructions avec cycles), `CYCLES_AUDIT.md`, `test/` (torture-tests `.s`/`.a09`).
   Compilateur : `~/cvs/gcc/gcc-16.2.0/CLAUDE.md` ; bibliothèque : `~/cvs/gcc/newlib-4.6.0/CLAUDE.md`.
4. **Le banc de non-régression VHDL** `~/vhdl/63F09/regress` : `regress.py`, `tests*.json`, `asm/*.asm`, générateurs
   `gen_*.py` (avec leurs modèles Python indépendants), `buslib.py` ; assembleur A09 `~/vhdl/63F09asmb/a09`.

## Sémantique à respecter (pièges déjà rencontrés)

Le modèle suit le VHDL **corrigé** (plusieurs défauts matériels ont été trouvés et corrigés ; un vieux comportement n'est pas
une référence). À reprendre un par un dans le VHDL au moment de coder, pas de mémoire :

- **Registres** : `PC S U X Y DP CC A B` partout ; `E F V` dès le 6309 ; `DS` en 63F09. Pseudo-registres `D=A:B`, `W=E:F`,
  `Q=D:W` (32 bits), `O=Q:V` (**64 bits**). En mode 63F09 32 bits, `PC S U X Y V` font 32 bits ; `D W DS` restent 16 bits.
- **`MD`** : `LDMD` doit être la **première** instruction (avant `S`), sinon TRAP ; valeurs `$01` (6309), `$11`/`$31`
  (63F09 32 bits sans/avec FPU). Codes de cause de TRAP dans `MD(7:6)`, `BITMD` (voir `63F09.vhd` rév. CVS 1.48).
- **`EXG`/`TFR`** et la famille `ADDR/ADCR/SUBR/SBCR/ANDR/ORR/EORR/CMPR` : deux registres de **même taille** uniquement.
  `OP r0,r1` range **`r1 <- r1 OP r0`** (donc `SUBR r0,r1 = r1 - r0`, `CMPR` compare `r1 - r0`).
- **Adressage direct 63F09** : `DS:DP:opérande`. `,R+`/`,R++` : l'incrément suit la largeur de la donnée en mode 32 bits.
  Vecteurs d'interruption : mots de 16 bits à `$FFFFFFC0`..`$FFFFFFFF`, le CPU ajoute le préfixe `$FFFF` (vecteur de reset `$F000`
  lu puis exécuté à `$FFFFF000`).
- **`PSHS`/`PULS`/`PSHU`/`PULU`** : octet de masque non signé en 16 bits (défaut VHDL corrigé le 2026-10-30).
- **`LDV` immédiat** : 63F09 32 bits seulement. `MULOU` : produit modulo 2^64 (64 pas toujours exécutés). Division par zéro :
  TRAP. `TFM` : repris après interruption, 4 formes, compte dans `W`.
- **Décalages/rotations de n bits** (63F09 seul, 84 mnémoniques) : encodages dans `CLAUDE.md` de binutils-gdb, rév. 67 ;
  `RORN`/`ROLN` sans retenue ; `C` = dernier bit sorti ; compte 0 ne change que `N` et `Z` ; octet non décodé = TRAP. Le modèle
  Python de `regress/proto/dmcnew/gen_shn.py` (`shn_model`) est une référence indépendante.
- **FPU** (`63F39.vhd` et en-tête de `63F09.vhd`) : pile de registres, formats S/D, niveaux de pile 1 à 16 pour
  `DUPNFP/DROPNFP/ROLLFP/ROLLDFP/PICKFP/EXGFP`, variantes **non dépilantes** `...NS/ND` (le résultat remplace le sommet,
  l'opérande du dessous est conservé), `FCMP*` range son résultat dans **`CC.Z` du CPU** (`CCFPU` garde `NV/DZ/OF/UF/NX`),
  `FNMADD = -(a*b) - c`, `FNMSUB = -(a*b) + c`, `PSHSNFP` etc. avec compte N, modes d'arrondi `SETRND*`, convention RISC-V
  (NaN canonique, `FCLASS`, `FSGNJ`, `FMIN/FMAX`). FPU **32 bits uniquement** (variante `63f09hf`). À relever : l'état de la
  pile FP quand le FPU est partagé entre cœurs (`FPU_BY_CORE = false`).
- **MMU 63F29** (lire les commentaires de `63F29.vhd` lignes ~425-660, ils sont la spécification) : transparente au reset ;
  `RING` (bit 7 = MMU active, bits 1:0 = anneau), `FUSE` (+1), `STATUS` (+2), `MAXRING` (+3), `TAG_STAGING` (+4..+7) ;
  fenêtres `TAG` `$100-$5FF`, `LRU` `$600-$7FF`, `INDIRECTIONS` `$800-$FFF` ; 4 anneaux, 64 indirections de 4 Ko par
  processus ; charge utile : bit 26 exécution, 25 écriture, 24 lecture, bits 23-0 page physique (36 bits au total).
  Écriture d'une indirection : tag d'abord, puis les 4 octets de charge utile, **le dernier valide** ; l'index écrit est
  **ignoré** : le matériel remplace l'entrée la plus ancienne non « toujours mappée ». Faute de page = TRAP vers l'anneau 0,
  causes dans `STATUS`. Après chaque push de pile (anneau > 0) le CPU écrit le drapeau « toujours mappé » et sonde `S-36`.
  **`FUSE`** : compteur décrémenté à chaque cycle de bus tant qu'il est non nul, interruptions masquées pendant ce temps,
  l'anneau effectif ne se resynchronise sur `RING` qu'à `FUSE = 0` ; la séquence type (écrire `RING`, `FUSE = N`, `JSR` vers
  un `RTI`) fait changer d'anneau **au milieu** de `RTI` : il faut le compte de cycles exact et l'ordre exact des accès de
  `RTI`/`PULS`/`JSR`/`RTS` (à confirmer ligne par ligne dans `63F29.vhd` ~1250-1265 et ~1530-1545).
- **Accès distant à la MMU** : le cœur 0 peut programmer la MMU d'un autre cœur **arrêté** (`HALT`, état `HALTED`) ; écriture
  complète, **lecture limitée aux registres `$000-$008`** (`RING FUSE STATUS MAXRING TAG_STAGING`, nombre d'indirections) ; les
  tableaux se relisent à `$FF`. Hors cet état, lecture `$FF` et écriture ignorée.
- **Carte mémoire** (physique, 36 bits, `~/cvs/gcc/github/63F09-emulator/docs/MEMORY_MAP.md`) : ROM 64 Ko `FFFFF0000`,
  RAM interne 64 Ko `FFFFC0000`, MMU `k` à `FFFFD0000 + k*1000h` (4 Ko), PIA0-4 `FFFFEF000+2k`, ACIA0/1 `FFFFEE000/4`, PTM0/1
  `FFFFED000/8`, SPI0 `FFFFEC000`, TWI0 `FFFFEB000`, BAC0 `FFFFEA000`, WDG0 `FFFFE9000` ; le reste va au bus externe EXT0 (le
  FPGA compagnon, 64 bits). Sans MMU, adresse physique = `$F` suivi des 32 bits logiques (un programme lié à 0 tourne à
  `F00000000`). BAC : écrire `$03/$07/$0F` dans `BAC0` réveille les cœurs (reset = `$01`) ; l'identité du cœur se lit à `BAC0+1`
  (à relever dans `63F81.vhd`).
- **Adresses en modes 16 bits (6809/6309)** : le VHDL actuel (`63F09.vhd`, processus de sortie d'adresse, ~l.5185) force
  `A(31 downto 16) <= x"FFFF"` quand `MD` n'est pas en 32 bits : **toute adresse 16 bits est dans le dernier segment
  `FFFF:xxxx`** (ROM et banc ROM, aucune RAM atteignable). Les vecteurs d'interruption ont toujours 16 bits
  (`REG_PC(31:16) <= x"FFFF"`). Un programme 16 bits ne peut donc lire que dans cette ROM et doit passer en mode 32 bits
  (`LDMD #$31`, légal tant que `S` n'est pas chargé) pour écrire ses résultats. L'ancienne incohérence (extension de signe
  selon l'instruction) a été corrigée le 2026-10-28 (CVS 1.49) ; l'ancien comportement n'est pas une référence.

## Architecture proposée

Répertoire **`63F09/`** ajouté à ce dépôt (aucun fichier amont modifié, sauf quelques lignes de `makefile` : variables
`M63F09D`, `M63F09`, cible `m63f09`, sur le modèle de `SWTP6800D` autour de la ligne 2387 et du bloc cible ~3045), compilation avec
`-DUSE_INT64 -DUSE_ADDR64` (`t_value` 64 bits pour `O`, `t_addr` 64 bits pour les 36 bits physiques). Squelette SCP copié de
`swtp6800/` (CPU 6800 déjà dans l'arbre).

- **Noyau fonctionnel portable** `m63f09_core.[ch]` : état d'un cœur, décodage, exécution, MMU, FPU ; **aucune dépendance
  SIMH** (interface : lecture/écriture mémoire physique, ligne d'interruption, horloge). Il pourra être enveloppé plus tard
  dans QEMU ou un autre hôte.
- **MMU dans un fichier à part** (`m63f09_mmu.[ch]`, partie du noyau portable ; VAX et 3B2 font de même, le PDP-11 non) : le
  chemin rapide (micro-TLB à correspondance directe, droits) est en `static inline` dans l'en-tête `m63f09_mmu.h`, car chaque
  accès mémoire le traverse ; le chemin lent (parcours des indirections, LRU, fautes de page, registres, protocole d'écriture,
  accès distant) est dans `m63f09_mmu.c`. Un `DEVICE` par MMU (`MMU0`..`MMU7`) expose ses registres à `examine`/`show`.
- **Colle SIMH** : `m63f09_cpu.c` (DEVICE/UNIT/REG, `sim_instr()`), `m63f09_sys.c` (`sim_devices`, `sim_load`, `sim_fprint_sym`),
  un fichier par périphérique (`m63f09_acia.c`, `_ptm.c`, `_pia.c`, `_spi.c`, `_twi.c`, `_wdg.c`, `_bac.c`).
- **Décodage généré** : `tools/gen_optab.py` lit `~/cvs/gcc/binutils-gdb/opcodes/m6809-opc.c` (page, opcode, mode, longueur,
  cycles, masque d'architecture) et produit `m63f09_optab.h` ; le même tableau sert au désassemblage de `examine -m` (aucune
  dépendance à BFD). Piège : une même (page, opcode) a plusieurs lignes selon l'architecture/largeur, et la famille décalages-n
  se décode à partir de l'octet postfixe (voir `opcodes/m6809-dis.c`).
- **Temps** : `sim_interval` décrémenté du nombre de cycles de l'instruction (`cycles_63F09`, minimums audités ; formules pour
  `PSHSNFP` etc. dans `CYCLES_AUDIT.md`). Fréquence du cœur en paramètre (objectif matériel 100 MHz, SYSCLK 24 MHz pour les
  périphériques : ACIA = `24e6 / (16 * (div + 1))`). Le cache DMC et la contention de bus ne sont pas modélisés.
- **8 cœurs, deux modes** (idée de l'utilisateur, 2026-11-10 : « un thread par cœur, si c'est possible, pourquoi s'en priver ? ») :
  (a) **déterministe** (défaut) : N cœurs ordonnancés à tour de rôle dans `sim_instr()` par quanta de cycles, courses
  reproductibles, mise au point simple ; (b) **un thread hôte par cœur** (`set cpu threads`), seule façon d'approcher la vitesse du
  matériel avec 8 cœurs actifs (8 x 43 MIPS = 344 MIPS, contre ~150 MIPS pour un seul thread). SIMH n'est pas multi-thread (file
  d'événements, `sim_interval`, `sim_debug`, `sim_printf`, points d'arrêt : rien n'est protégé), donc **le noyau fonctionnel doit être
  écrit pour le permettre dès le départ** : aucune variable globale (un `core_t` par cœur, MMU comprise) ; mémoire physique
  accessible par des accesseurs qui peuvent devenir atomiques (le TSO de x86-64 est proche du matériel cohérent) ; `LOCK`/`UNLOCK`
  par un verrou hôte ; interruptions et `HALT`/`DMA` par drapeaux atomiques ; accès aux **périphériques sous un verrou global**
  (comme le BQL de QEMU), jamais par la file d'événements SIMH depuis un autre thread ; seul le cœur 0 (le thread de SIMH) appelle
  `sim_activate`, `sim_debug` et le débogueur ; arrêt par un drapeau testé tous les N cycles puis barrière. Temps : chaque cœur a
  son compteur de cycles, resynchronisé par barrière à chaque quantum (écart borné, plus déterministe). À faire après M5
  (+4 à 8 jours) et seulement après avoir mesuré le mode (a).
- **MMU** : une instance par cœur ; micro-TLB à correspondance directe devant les 64 indirections, vieillissement LRU
  reproduit exactement (algorithme à relever dans `63F29.vhd`) ; restauration de l'état d'une instruction fautive (registres
  validés **après** l'accès mémoire).
- **FPU** : commencer avec `<fenv.h>` de l'hôte (x86-64 SSE) pour flags, arrondis et FMA ; basculer sur Berkeley softfloat
  seulement si l'émulateur RTL révèle des écarts (NaN, drapeaux).
- **Compagnon FPGA** (écran, réseau, audio I2S ; interface **pas encore spécifiée**) : bibliothèque C autonome (lecture/écriture
  d'adresse, un pas de temps, pointeur vers l'image), partagée avec l'émulateur RTL (`--ext`, « companion interface » prévue
  dans son README). Côté SIMH : `sim_video` (SDL2, `vid_draw`, `vid_poll_kb`, `vid_poll_mouse`, `vid_screenshot` pour des tests
  graphiques), `sim_ether` (PCAP/TAP/VDE/slirp), disques via `sim_disk`. Pas d'API audio dans SIMH : à ajouter (SDL audio).
  **Écrire d'abord la spécification d'interface du FPGA.**
- **gdb** : stub RSP (~500 lignes) à écrire ; le numérotage des registres doit suivre `gdb/m6809-tdep.c` de binutils-gdb.

## Jalons et critères de réussite

| Jalon | Contenu | Critère |
|---|---|---|
| M0 | squelette, `make m63f09`, chargeur ELF, lecture du vecteur de reset | `sim> boot` s'exécute, `examine` désassemble |
| M1 | jeu d'instructions 63F09 en mode 32 bits + exceptions + ACIA0 sur la console (critère révisé : un programme 16 bits ne voit que `FFFF:xxxx` sans MMU) | `newlib_hello` compilé `-m63f09` s'affiche sur l'ACIA0 ; tous les oracles identiques à `soc63f09` |
| M2 | 63F09 32 bits complet (modes, `TFM`, mul/div, décalages-n, bit-transfer) | programmes de `regress` : mémoire finale identique à l'émulateur RTL |
| M3 | MMU, anneaux, interruptions, `TRAP`, `FUSE` | tests `mmuR*`, `ring*` de `regress` |
| M4 | FPU | `dir_fnma`, tests FPU de `regress`, `test/fpu_*` |
| M5 | SMP, BAC, `HALT`/`DMA`, tous les périphériques | tests `dir_*` fonctionnels (sans les critères de cache) |
| M6 | stub gdb | `gdb` + `m6809-tdep` : point d'arrêt réellement atteint (jamais testable jusqu'ici) |
| M7 | compagnon : écran, réseau, audio | selon la spécification du FPGA |
| M8 | systèmes (FLEX9, UniFLEX, NetBSD) | démarrage |

**Méthode de validation** (prévoir dès M1) : un script `tools/diff_rtl.py` lance le **même ELF** dans SIMH et dans
`soc63f09 --dump`, compare les plages mémoire surveillées (`watched` des `tests*.json`) ; les sorties des générateurs
`gen_*.py` (modèles Python indépendants) servent d'attendus. Corriger sur preuve (octet réel), jamais par relecture seule.

## Estimation (jours de travail concentré, ±40 %, hors compagnon)

Squelette et décodeur 2-3 ; état du CPU 2-3 ; **jeu d'instructions 22-32 (le plus gros, surtout de la vérification)** ; MMU 5-7 ;
interruptions/TRAP/instructions reprenables 3-4 ; `FUSE` et temps 3-4 ; FPU 4-5 ; périphériques 5-8 ; SMP 3-4 ; stub gdb 2-3 ;
validation différentielle 5-8. **Total 56 à 81 jours.** Compagnon, à part : écran + clavier/souris 3-5, réseau 3-5, audio I2S
2-4, blitter 2D 3-6 s'il existe.

## Périphériques externes prévus et démarrage (informations de l'utilisateur, 2026-11-10)

À connecter au SoC par le SPI (`63F52`, maître : **16 sorties `SS`**, `CPOL`/`CPHA` et diviseur d'horloge de 16 bits dans les
registres de contrôle, donc suffisant pour les deux composants) :

- **RTC Epson RX4111CE** (fiches dans `~/datasheets/horloges/`) : SPI 4 fils, **4 MHz maximum**, bascule automatique sur
  batterie (`VBAT`), calendrier complet, alarme, minuteur de réveil, sortie 32,768 kHz, **horodatage de 8 événements** dont un
  déclenché par accès SPI, 64 octets de mémoire utilisateur ; registres en banques. À relever dans la fiche : mode SPI
  (`CPOL`/`CPHA`), indicateurs de perte d'alimentation/arrêt de l'oscillateur, ligne d'interruption.
- **MRAM Everspin MR25H256A** (`~/datasheets/memoires/`) : 32 Ko (32K×8), SPI modes 0 et 3, 40 MHz maximum, sans délai
  d'écriture ni limite d'endurance ; commandes `WREN 06h`, `WRDI 04h`, `RDSR 05h`, `WRSR 01h`, `READ 03h`, `WRITE 02h`
  (adresse sur 2 octets), `SLEEP B9h`, `WAKE ABh` ; registre d'état `SRWD`/`BP1`/`BP0`/`WEL`, broche `WP`. Sert à **garder la
  configuration**.

**Démarrage prévu** : le SoC utilise sa ROM de 64 Ko et sa RAM de 64 Ko pour charger une **console de type SRM** qui charge
l'OS depuis le réseau, un disque ou autre support **dans la mémoire externe (`$0:0000:0000`)** et **mappe de nouvelles
interruptions** (la table de vecteurs en ROM, `$FFFFFFC0`, ne doit pas rester celle de l'OS : la MMU permet de remplacer la
page des vecteurs). Le démarrage est **multi-fichiers** (noyau + serveurs indispensables).

Conséquences pour ce simulateur :

- Modèles de **RTC** et de **MRAM** sur le SPI (deux sélections de puce), avec persistance : le contenu de la MRAM dans un
  fichier (`attach`), l'heure de la RTC sur celle de l'hôte ou figée pour des tests reproductibles.
- Le simulateur doit pouvoir **exécuter la vraie console** (ROM) et non seulement charger un noyau en mémoire : `sim> boot`
  part du vecteur de reset, comme le matériel. Le chargeur ELF direct (`load`) reste un raccourci pour les tests.
- Réseau et disque : `sim_ether` pour un démarrage réseau, `sim_disk` pour un démarrage disque.
- La console, côté logiciel, doit traiter le **démarrage multi-fichiers**, c'est-à-dire **Multiboot** (modules de démarrage :
  noyau + serveurs indispensables, liste de modules passée au noyau) ; terme retenu avec l'utilisateur le 2026-11-10. Les cas
  dégradés de la MRAM et de la RTC (vierge, corrompue, absente, protégée, heure invalide) restent à confirmer.
- Impact binutils/`ld` (voir `~/cvs/gcc/binutils-gdb/CLAUDE.md`, « Reste à faire » n° 3) : la console est une seconde image
  ROM liée à part de l'amorce (`-T`, émulation `m6809soc`), les noyaux et serveurs sont liés pour la mémoire externe ; un
  démarrage Multiboot demande une liste de modules passée au noyau (adresses et tailles des fichiers chargés en mémoire).

## Points ouverts (à trancher avec l'utilisateur)

- **Précision du temps** : proposé fonctionnel + `FUSE` exact, sans cache DMC.
- **MMU** : figer la 63F29 actuelle ; le cahier des charges des contextes (SAVE/LOAD/CLEAR dans le BAC) est en v21 sans VHDL.
- **Interface du compagnon FPGA** : à spécifier avant d'écrire le modèle.
- **FPU partagé** : état de la pile FP entre cœurs à relever.
- **Licence** : `LICENSE.txt` est de type MIT mais ajoute des contraintes par fichier ; à lire en entier avant de reprendre du
  code, conserver les mentions d'origine.
- **Allègement de l'arbre** (question de l'utilisateur, 2026-11-10) : voir ci-dessous.

## Allègement de l'arbre (fait le 2026-11-10 sur la branche `63f09`, non commité)

Décision de l'utilisateur : un seul simulateur, pas l'ensemble. Fait après la lecture (`NOTES_63F09_LECTURE.md`) : suppression
de 38 répertoires de simulateurs (dont `display`), de `Visual Studio Projects`, `.github`, `appveyor.yml`, des `build_*.bat` et
de `descrip.mms`. **Gardés pour l'instant** : `swtp6800` (gabarit), `VAX` et `PDP11` (références MMU, vidéo, réseau ; le VAX
compile avec des fichiers du PDP-11), `AltairZ80` (ACIA de type 6850), `slirp`, `slirp_glue`, `frontpanel`, `doc`. À retirer
une fois le squelette 63F09 en place : `VAX`, `PDP11`, `AltairZ80`, puis `swtp6800`. 554 fichiers suivis (de 1 689), 56 Mo (de 125).
`makefile` : seule la variable `ALL` est réduite (l'arbre d'origine est en CRLF ; **converti en LF le 2026-11-10 sur demande de l'utilisateur**, 430 fichiers texte, binaires intacts, `* text=auto eol=lf` ajouté à `.gitattributes` ; pour relire les vraies modifications, `git diff --ignore-cr-at-eol`) ;
`sim_BuildROMs.c` : retrait des 4 entrées de `SDS` (l'outil échoue si un fichier listé manque). Vérifié : `make swtp6800mp-a2`
compile et tourne ; répondre `n` à la question sur `libpcre3-dev`/`libedit-dev` (inutile : PCRE et EditLine sont chargés à
l'exécution par `dlopen`, `EXPECT` fonctionne). Les fusions avec l'amont produiront des conflits « modifié/supprimé » dans les
répertoires supprimés : les résoudre par `git rm`.

## Avancement

- **2026-11-10 : générateur du décodeur** `63F09/tools/gen_optab.py` (hors `tools/` racine pour ne rien ajouter à l'arbre amont)
  -> `63F09/m63f09_optab.h` (1 420 lignes, une par mnémonique et mode, avec le masque d'architecture et les cycles ; le
  nombre de `M6809_INSN` du source est vérifié, constantes `ADDR_*`/`ARCH_*` recopiées sous le préfixe `M63_`). Relancer le
  script après tout changement de `opcodes/m6809-opc.c`.
- **Décodage par état du CPU** (mesuré sur la table) : opcodes distincts 269 (6809), 437 (6309), 755 (63F09), 879 (63F09
  avec FPU). Pour un état donné, un (page, opcode) n'a plusieurs lignes que dans trois cas : alias de même encodage
  (`asl`/`lsl`, `bhs`/`bcc`, `lbhs`/`lbcc`, `asla`/`lsla`), indexé contre indexé indirect (séparés par le bit 4 de l'octet
  postfixe), et la famille décalages-n (le registre est dans le mnémonique, l'octet postfixe décide). Le décodeur peut donc
  être une table `[page][opcode]` par état, construite au démarrage, sans backtracking.
- **2026-11-10 : M0 fait** (critère : `boot` s'exécute, `examine -m` désassemble). Fichiers de `63F09/` : `m63f09_defs.h` (carte
  physique de 36 bits relevée dans `SoC.vhd`, état d'un cœur, codes d'arrêt), `m63f09_cpu.c` (DEVICE `CPU`, registres, ROM 64 Ko en
  `0xFFFFF0000`, RAM 64 Ko en `0xFFFFC0000`, mémoire externe optionnelle `SET CPU EXTMEM=n` en `0xF00000000`, reset par le vecteur
  `0xFFFFFFFE` + `0xFFFF` devant, `sim_instr` qui décode puis s'arrête « non implémenté »), `m63f09_sys.c` (chargeur ELF32
  grand-boutiste par les **sections** à leur adresse de chargement tirée des en-têtes de programme ; chargeur binaire brut ;
  `fprint_sym`), `m63f09_dis.c` (port du désassembleur de binutils, même syntaxe). Cible `make m63f09`.
- **Compiler** : `echo n | make -j5 m63f09` (la question sur `libpcre3-dev`/`libedit-dev` bloque `make` sinon, y compris en tâche de
  fond). Lancer : `BIN/m63f09 63F09/tests/m0.ini`.
- **Décodage = MD, pas l'architecture de l'assembleur** : le noyau décode selon `MD` (0 = 6809, bit 0 = 6309, bit 4 = 63F09 32 bits,
  bit 5 = FPU) ; les rangs de la table disent quelle cible de l'assembleur accepte une instruction. Une exception vue dans le VHDL
  (`63F09.vhd`, `113D`) : **`LDMD` immédiat est décodé dans tous les états**, sinon rien ne sortirait de l'état 6809 du reset.
  `m63f09_find_op()` l'applique ; l'exécuteur de M1 doit l'utiliser. `examine -m` suit `MD` (donc l'état 6809 avant le `LDMD`) :
  `SET CPU DISASM=63F09` force un autre état.
- **Validation du désassembleur** : `63F09/tests/check_dis.py` assemble les `test/*.s` de binutils, les charge en ROM et compare
  `examine -m` à `objdump -d` (adresses, donc longueurs, et mnémoniques) : 35 fichiers sur 36 identiques. L'écart restant est
  `pseudo_data.s`, où `objdump` lui-même échoue (« Address 0x9 is out of bounds », opérande qui traverse une étiquette). SCP fusionne
  les lignes identiques consécutives de `examine` en « A: thru B: same as above. » (pas d'option pour l'éviter) : le script les développe.
- **2026-11-10 : exécuteur du jeu de base (M1)** `63F09/m63f09_exec.c` : classification des lignes de la table par mnémonique
  (une seule fois), décodage par état `MD`, modes d'adressage (indexé avec pas de `,R++`/`,--R` selon la largeur de la donnée, 1/2/4/8), ALU par
  largeur, chargements/stockages sur tous les registres, branchements, `LEA`, `TFR`/`EXG`, registre-registre, `PSH`/`PUL` (ordre du VHDL),
  `TFM`, bit-transfer, `OIM`..., `DAA`, `SEX`/`SEXW`, `MUL`. **Validation différentielle** : `63F09/tests/run_isa.py` (programmes `isaN.asm` de
  `regress`, A09 -> ELF par `tools/s09_to_elf.py`, exécutés par `soc63f09` et par `BIN/m63f09`, 96 octets de résultat comparés par cas) : 12
  parties sur 13 identiques (563 cas). Reste la partie 12 (`MULD`/`DIVD`/`DIVQ`, pas encore écrits). `M63_RTL_ADCR8_NO_CARRY` vaut 0 (le VHDL est
  corrigé, voir binutils-gdb/CLAUDE.md révision #68) : `soc63f09` n'est pas régénéré, d'où 3 écarts attendus sur la partie 10.
- **2026-11-10 : partie 12 faite** (`MULD`, `DIVD`, `DIVQ`) : les 13 parties de l'oracle ISA sont identiques à `soc63f09` (613 cas). En 32 bits
  `DIVD` = D / M16 (quotient dans W, reste dans D), `DIVQ` = Q / M32 (quotient dans Q, reste dans V), V toujours à 0 ; en 16 bits : règles
  du manuel (non vérifiées par l'oracle, qui tourne en 32 bits).
- **2026-11-10 : multiplications et divisions non signées et 64 bits faites** (`MULDU`, `MULQ`, `MULQU`, `MULO`, `MULOU`, `DIVDU`, `DIVQU`, `DIVO`,
  `DIVOU`) : `63F09/tests/run_wide.py` (320 cas, 4 formes d'adressage, plusieurs valeurs de signe et de grandeur, 2 CC initiaux) identique à
  `soc63f09`. Résultats lus dans le VHDL : `DIVDU` quotient dans W et reste dans D ; `DIVQU` quotient dans Q et reste dans V ; **`DIVO`/`DIVOU` quotient
  dans O et reste POUSSÉ sur la pile U (8 bytes)** ; drapeaux des divisions : N Z du quotient, C = son bit 0, V = 0 ; drapeaux des multiplications :
  règle du propriétaire (`regress/gen_mulflags.py`). Les programmes `dir_*` de `regress` utilisent le BAC et une boîte aux lettres entre cœurs :
  inutilisables tant que le BAC n'est pas modélisé (`63F09/tests/run_prog.py` est prêt pour ce jour-là).
- **2026-11-10 : décalages et rotations de n bits** (84 mnémoniques) : `63F09/tests/run_shn.py`, 1 920 cas (12 registres, 5 opérations, comptes 0/1/2/
  max/2/max, valeurs de signe variées, 2 CC, et l'octet en mémoire en 3 formes) identique à `soc63f09`. **Instructions isolées** : `LDDP`/`LDDS`/
  `STDP`/`STDS`/`SEXV`/`ENI`/`DII`/`LOCK`/`UNLOCK` (`run_misc.py`, 176 cas identiques) ; `HCF` arrête le simulateur ; `LOCK`/`UNLOCK` sont sans effet avec un seul
  cœur. Le registre `MD` a 24 bits de masque d'interruptions au-dessus des bits de mode (`imask` de `m63f09_core_t`).
- **Couverture** (`63F09/tests/coverage.py`) : **546 mnémoniques sur 546 traités** (2026-11-11). Les opérations arithmétiques sur `O`, `V`, `Q`, `W` passent par les familles
  génériques mais ne sont PAS exercées par l'oracle `isaN` (pas de cas `O`) : à valider avec `gen_mulou.py`, `gen_mulflags.py`, `gen_idxm.py`, `gen_random.py` de `regress`. Pas encore : `FUSE`, MMU.
- **`TFM` recharge les drapeaux par le décrément de W** (RTL : `BLOCK_TRANSFER_STATE`, `W - 1` avec `CC_CTRL <= LOAD_CC` à chaque itération). Vérifié
  par `63F09/tests/gen_tfmcc.py` (80 cas, RTL et simulateur identiques) : avec W >= 1 le CC final a **Z = 1** et N = V = C = 0, H I F E inchangés ;
  avec W = 0 le CC est inchangé. **Décision de l'utilisateur (2026-11-10) : on laisse tel quel** ; N, V, C ne sont PAS garantis par `TFM` (l'ALU décrémente W, ils sont nuls en pratique). Reproduit (`M63_RTL_TFM_LOADS_CC`), à ne pas considérer comme un défaut.
- Découvertes de la validation : voir binutils-gdb/CLAUDE.md révision #68 (`ADCR`, `STBT`).
- Critère M1 révisé : un programme 16 bits ne voit que le segment `FFFF:xxxx` (63F09.vhd 1.49) et ne peut atteindre RAM et périphériques qu'avec
  la MMU configurée : M1 vise donc le jeu de base en mode 32 bits ; le 16 bits avec périphériques relève de M3.
- **2026-11-11 : exceptions faites** (`m63f09_exec.c`, section « Exceptions »), lues dans `63F09.vhd` (`INTERRUPT_*`, `RTI_*`, `CWAI_*`, `SYNC_STATE`, `ERROR_STATE`) : `SWI`, `SWI2`, `SWI3`,
  `RTI`, `CWAI`, `SYNC`, `BITMD`, les lignes `IRQ`/`FIRQ`/`NMI`/`EIRQ0..23` (priorité NMI, FIRQ, IRQ, EIRQ23 à EIRQ0 ; masques `CC.I`/`CC.F` et `imask` ; EIRQ sur niveau ou sur front selon `MD.ED` ;
  `FIRQ` en trame courte quand `MD.FM` = 0), `TRAP` (instruction illégale `MD(7:6)` = 01, division par zéro 10 avec les drapeaux de `CLR` empilés, `TFR`/`EXG` de tailles différentes, octet de
  décalage-n non décodé, **`LDMD` après le chargement de `S`**). Trames : 16 bits `PC [U Y X DP (F E) B A] CC`, 32 bits `PC [V U Y X DP F E B A DS] MD(4 octets, octet de mode en premier) CC` ; vecteur
  = mot de 16 bits en `$FFFFFFC0 + 2n`, `$FFFF` mis devant en 32 bits. **Oracle** `63F09/tests/run_exc.py` : `SWI`/`SWI2`/`SWI3`/`RTI` et les divisions par zéro (trame de 32 octets, S, tag, `BITMD`
  du handler, registres après retour) : 24 cas identiques à `soc63f09` ; les `TRAP` d'instruction illégale n'ont **pas de référence** (`ERROR_STATE` contient un `finish;` : la simulation RTL
  s'arrête avant d'empiler) : 54 cas vérifiés seulement dans le simulateur (tag, bit `E`, `MD(7:6)` = 01), **le PC empilé n'est pas vérifié contre le RTL** (le simulateur empile le PC après les
  octets lus quand la cause est trouvée). **16 bits** : `run_exc16.py` (6809 et 6309, `SWI`/`SWI2`/`SWI3`/`RTI` et une `FIRQ`, trames contrôlées octet par octet contre le VHDL ; un cœur 16 bits ne voit que
  la ROM : `SET CPU ROMWR` la rend inscriptible pour ce test) ; **lignes d'interruption** : `run_irq.py` (IRQ, CWAI + IRQ, SYNC avec IRQ masquée, IRQ masquée, FIRQ courte, NMI armé par le premier
  chargement de `S`, EIRQ3 sur niveau ; les lignes sont pilotées par les registres `IRQLINES` et `NMIPEND` du CPU, `soc63f09` ne les lève que par `--pins`). `CWAI` 32 bits : l'immédiat est de 32 bits, les 8
  bits bas masquent `CC`, les 24 autres **remplacent** `imask`. **Piège SCP** : `boot cpu` *lance* le programme (il faut `reset` puis `go` quand on pose des lignes avant l'exécution).
- **2026-11-11 : FPU fait** (`m63f09_fpu.c` pour l'unité arithmétique, `do_fpu()` dans `m63f09_exec.c` pour la pile de 16 registres de 64 bits, les chargements/rangements, `PSH*FP`/`PUL*FP` et leurs formes N,
  `TFROFP`/`TFRFPO`, les branchements `BNV`... et `FCMP` qui répond dans le `Z` du CC). L'arithmétique est celle de l'hôte (`fenv`) : RNE, RTZ, RDN, RUP ; **RMM** (égalité vers le plus grand module)
  est reconstituée en détectant les égalités exactes ; la **valeur 7 (SETDYN) se comporte comme RTZ** pour la mantisse mais donne l'infini en cas de dépassement (mesuré). Écarts du RTL avec RISC-V trouvés
  par l'oracle : `LDSFP` étendait le signe sur 64 bits (corrigé dans le VHDL le 2026-11-11) ; les résultats de conversion en entier 32 bits sont étendus par des zéros ; une conversion invalide (NaN, hors plage) donne 0x80..0 en signé et tous
  les bits à 1 en non signé (même pour un négatif) ; `FMAX` avec `rm` > 1 donne 0 et aucun drapeau ; `FMAX` rend l'opérande tel qu'il est dans le registre (64 bits) ; les NaN résultats sont canoniques.
  **Oracles** `run_fpu.py N seed` (arithmétique, conversions, tous modes d'arrondi, valeurs spéciales, 4 x 1 200 cas) et `run_fpu2.py N seed` (pile, `LDSFP`/`STDFP` dans tous les modes d'adressage dont `,X++`,
  `PSH*`/`PUL*`, branchements sur drapeaux, 2 x 900 cas) : voir le tableau. **`soc63f09` exige 2 cœurs et un FPU partagé** (`--ncpus 2 --fpu shared`, valeurs par défaut) : avec 1 cœur ou `by-core` la
  première opération FPU ne rend jamais la main.
- **2026-11-11 : `hello world` complet** : `m6809-unknown-elf-gcc -m63f09hf -msoc -Os hello.c` (`puts` et `printf("%d + %d = %d\n")` de newlib) s'affiche sur l'ACIA0 (`Hello, world!` / `40 + 2 = 42`),
  54 882 octets de code ; en `-m63f09` seul le lien échoue (la ROM de 64 Ko déborde de 5 671 octets : libgcc en virgule flottante logicielle).
- **2026-11-11 : entrée de l'ACIA par un tube et interruption de réception** (`m63f09_acia.c`) : SCP ne scrute la console que si stdin est un terminal (`sim_poll_kbd`) ; le modèle lit lui-même un stdin qui n'en est pas un
  (`poll()` puis `read`, la fin de l'entrée est mémorisée ; la commande `SEND` de SCP continue de marcher). Un caractère n'est pris que si le récepteur a été lu, puis le suivant attend la durée d'un caractère. L'interruption
  de l'ACIA0 est **EIRQ23**, celle de l'ACIA1 EIRQ22 (`SoC.vhd`), niveau actif tant que `(RIE et RDRF) ou (TIE et TDRE)` (drapeau du registre d'état) ; vecteur `$FFFFFFEE` pour EIRQ23. `run_acia.py`.
  **Piège** : après un `reset` le diviseur vaut `$FFFF` (environ 23 bauds) : un programme qui ne le programme pas voit un caractère toutes les 0,4 s de temps simulé.
- Reste à faire de M1 : voir la section suivante.

## M1 : état et reste à faire (2026-11-11)

**Définition retenue de M1** : le jeu d'instructions du 63F09 (6809, 6309, 63F09 32 bits **et FPU**), les exceptions et la console série, validés par des oracles contre `soc63f09` (l'émulateur RTL)
quand le RTL peut les jouer. **M1 est fonctionnellement terminé** : les 546 mnémoniques sont exécutés et `hello world` s'affiche. Ni la MMU ni le SMP (M2 et suivants).

### Fait et validé (lancer depuis la racine de l'arbre : `python3 63F09/tests/<script>`, ou `63F09/tests/run_all.sh`)

| Oracle | Contenu | Cas identiques à `soc63f09` |
|---|---|---|
| `run_isa.py` | jeu de base : ALU 8/16/32 bits, chargements/stockages, branchements, `LEA`, `TFR`/`EXG`, registre-registre, `PSH`/`PUL`, `TFM`, bit-transfer, `OIM`..., `MULD`/`DIVD`/`DIVQ`, appels | 613 / 613 |
| `run_wide.py` | `MULDU` `MULQ` `MULQU` `MULO` `MULOU` `DIVDU` `DIVQU` `DIVO` `DIVOU` (4 formes d'adressage) | 320 / 320 |
| `run_shn.py` | décalages et rotations de n bits, 12 registres et l'octet en mémoire | 1 920 / 1 920 |
| `run_misc.py` | `LDDP` `LDDS` `STDP` `STDS` `SEXV` `ENI` `DII` `LOCK` `UNLOCK` | 176 / 176 |
| `run_exc.py` | `SWI` `SWI2` `SWI3` `RTI`, divisions par zéro (trame, MD, drapeaux) ; + 54 `TRAP` illégaux sans référence RTL | 24 / 24 (+ 54 contrôlés) |
| `run_fpu.py` | opérations arithmétiques, comparaisons, conversions, arrondis, valeurs spéciales (seeds 1 à 4) | voir la fin du fichier (résultats du jour) |
| `run_fpu2.py` | pile FPU, `LDSFP`... tous modes, `PSH*FP`/`PUL*FP`, `TFROFP`/`TFRFPO`, branchements FPU | voir la fin du fichier |
| `gen_tfmcc.py` | CC juste après `TFM` | 80 / 80 |
| `check_dis.py` | désassembleur contre `objdump` (36 fichiers `test/*.s` de binutils) | 35 / 36 (le 36e : échec d'`objdump` lui-même) |
| `run_exc16.py` | 6809/6309 : `SWI` x3, `RTI`, `FIRQ` (sans référence RTL : un cœur 16 bits ne voit que la ROM) | auto-contrôle, OK |
| `run_irq.py` | lignes IRQ/FIRQ/NMI/EIRQ, `CWAI`, `SYNC` (pilotées par script) | auto-contrôle, 7 / 7 |
| `run_acia.py` | ACIA0 : entrée par un tube sur stdin (écho par scrutation) et interruption de réception sur EIRQ23 (écho par le gestionnaire) | auto-contrôle, 2 / 2 |

Mesuré : **43 MIPS** sur une boucle serrée. Programmes GCC exécutés : `gcc-16.2.0/test/soc_hello.c` (`counter = $61`, comme `soc63f09`), `newlib_hello` (`iprintf`), `hello.c` complet (`puts` + `printf`).

**Écarts RTL/manuel reproduits** (macros en tête de `m63f09_exec.c`) : `M63_RTL_ADCR8_NO_CARRY` = 0 (VHDL corrigé le 2026-11-10) ; `M63_RTL_TFM_LOADS_CC` = 1 (accepté par le propriétaire).

### Reste à faire (hors M1)

1. **Non vérifié contre le RTL** : le PC empilé par un `TRAP` d'instruction illégale (la simulation RTL s'arrête dans `ERROR_STATE`) ; la trame `FIRQ`/`IRQ` d'un cœur 16 bits (impossible sur le banc) ; les lignes d'interruption
   (jouables avec `soc63f09 --pins`, socket Unix) ; `SYNC` avec une interruption non masquée (le délai de 3 cycles de la ligne n'est pas modélisé) ; les cas rares de `RMM` pour `FMADD`... en double et
   les conversions `CVTD2S` (les égalités exactes sont traitées pour +, -, *, / et les conversions entières, pas pour les FMA en double) ; `DIVD`/`DIVQ` en mode 16 bits (règle du manuel).
2. **Périphériques** (ACIA0 : entrée et sortie, interruption de réception câblée sur EIRQ23 ; ACIA1 sur EIRQ22 mais non connectée à la console) : `PTM` (63F40, c'est lui qui donne le temps), `PIA` (63F21), `SPI` (63F52) avec la RTC et la MRAM, `TWI`, `WDG`, et le **BAC (63F81)** : registres, boîte aux lettres, autres cœurs. Le BAC débloque
   les programmes `dir_*` de `regress` (`run_prog.py` est prêt). Seules l'ACIA0 et l'ACIA1 existent.
3. **MMU** (M2) et **SMP** (M3) ; `FUSE`.
4. **Validations manquantes** : arithmétique ordinaire sur `O`, `V`, `Q`, `W` ; formes `,R++`/`,--R` avec les pas 1/2/4/8 sur `LDO`/`STO` ; la pile `PSH`/`PUL` avec `DS`, `V`, `E`, `F` dans tous les sens.
5. **Temps** : `sim_interval` est décrémenté du minimum de cycles de la table ; pas de `FUSE`, pas de cache DMC, pas de contention de bus ; les instructions FPU utilisent les cycles de la table (mesurés en RTL).
6. **Outillage** : historique des instructions (`SET CPU HISTORY`), `DEBTAB`, désassemblage de la ligne en cours à l'arrêt.
7. **Performance** (après la correction fonctionnelle) : accès RAM direct par pointeur (en-tête `static inline`, comme le chemin rapide du VAX), décodage par table de fonctions, indicateur d'événement unique.

### Décisions ou informations attendues du propriétaire

- La carte de la **mémoire externe** (provisoirement en `0xF00000000`).
- Valeur de `MD(31:8)` au **reset** (0 dans le simulateur).
- `SETDYN` (`$1805`) : **retiré** (décision du propriétaire, 2026-11-11), l'opcode est réservé et déclenche un `TRAP` (VHDL, binutils, A09, simulateur).
- `FMAX`/`FCMP`/`FSGNJ` sont choisis par le registre de mode d'arrondi : **comportement voulu** (propriétaire, 2026-11-11), à documenter avec tous les opcodes. `FMAX` donne le **minimum** sous `SETRNE` (valeur du reset), le maximum sous `SETRTZ`, 0 sans drapeau sinon.
- Les conversions flottant vers entier invalides (NaN, hors plage, négatif vers non signé) donnent 0x80..0 en signé et tous les bits à 1 en non signé, résultats 32 bits étendus par des zéros : **à discuter** (le cas « négatif vers non signé donne tous les bits à 1 » est le plus surprenant). Les 32 bits de poids fort d'un simple sont ignorés (`LDSFP` étend le signe, sans gravité).

## Règles de travail

- Répondre en français ; messages de commit en anglais, avec la ligne d'attribution habituelle.
- **Toujours demander avant de commiter** ; travailler sur une branche `63f09`, `master` suit l'amont.
- Chaque nouveau fichier source du port porte la ligne de copyright « Dr. BERTRAND Joël » et Claude, ajoutée à la création.
- Ne pas modifier les fichiers amont sauf le `makefile` (quelques lignes), pour garder la fusion avec `simh/simh` simple.
- Construire avec `make -j5 m63f09`. Fichiers durables sous `~/cvs/gcc` (jamais dans `/tmp`).
- Un travail lourd à la fois ; ne pas lancer de simulation VHDL en parallèle d'une qualification en cours.
