/*
 * formant_tts.c  –  Formant-based Text-to-Speech synthesizer
 *
 * Architecture:
 *   Text → G2P (grapheme-to-phoneme rules) → Phoneme sequence
 *        → Phoneme sequencer (timing, amplitude envelopes)
 *        → Glottal source + formant filters + noise source
 *        → SDL2 audio output
 *
 * Phoneme set (ARPABET-like):
 *   Vowels:  AA AE AH AO AW AY EH ER EY IH IY OW OY UH UW
 *   Nasals:  M  N  NG
 *   Stops:   B  D  G  P  T  K
 *   Fric:    F  V  TH DH S  Z  SH ZH HH
 *   Affric:  CH JH
 *   Approx:  L  R  W  Y
 *
 * Build:
 *   gcc formant_tts.c -o formant_tts $(sdl2-config --cflags --libs) -lm
 *
 * Usage:
 *   ./formant_tts "Hello Dr. Falken. How is Joshua?"
 *   ./formant_tts          (interactive: type sentences, Enter to speak, Q to quit)
 */

#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Constants
 * ═══════════════════════════════════════════════════════════════════════════ */
#define SAMPLE_RATE   44100
#define BUFFER_SIZE   512
#define NUM_FORMANTS  4          /* F1..F4 */
#define TWO_PI        (2.0*M_PI)
#define MAX_PHONEMES  1024
#define MAX_WORD      64

typedef struct DictNode {
    char *word;
    char *phones;
    struct DictNode *next;
} DictNode;

#define DICT_BUCKETS 4096
static DictNode *cmu_dict[DICT_BUCKETS];


/* ═══════════════════════════════════════════════════════════════════════════
 * Biquad filter
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct { double b0,b1,b2,a1,a2,x1,x2,y1,y2; } Biquad;

static void biquad_bp(Biquad *f, double freq, double bw) {
    if (freq < 1.0)  freq = 1.0;
    if (bw   < 10.0) bw   = 10.0;
    double w0    = TWO_PI * freq / SAMPLE_RATE;
    double Q     = freq / bw;
    double alpha = sin(w0) / (2.0 * Q);
    double a0    = 1.0 + alpha;
    f->b0 =  alpha / a0; f->b1 = 0.0; f->b2 = -alpha / a0;
    f->a1 = -2.0*cos(w0) / a0;
    f->a2 = (1.0 - alpha) / a0;
}

/* Low-pass biquad (for noise shaping / fricative colour) */
static void biquad_lp(Biquad *f, double freq) {
    double w0    = TWO_PI * freq / SAMPLE_RATE;
    double alpha = sin(w0) / (2.0 * 0.707);
    double a0    = 1.0 + alpha;
    f->b0 = (1.0-cos(w0))/(2.0*a0); f->b1 = (1.0-cos(w0))/a0; f->b2 = f->b0;
    f->a1 = -2.0*cos(w0)/a0; f->a2 = (1.0-alpha)/a0;
}

static inline double bq(Biquad *f, double x) {
    double y = f->b0*x + f->b1*f->x1 + f->b2*f->x2 - f->a1*f->y1 - f->a2*f->y2;
    f->x2=f->x1; f->x1=x; f->y2=f->y1; f->y1=y;
    return y;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Phoneme definitions
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef enum {
    /* Vowels */
    PH_AA, PH_AE, PH_AH, PH_AO, PH_AW, PH_AY,
    PH_EH, PH_ER, PH_EY, PH_IH, PH_IY,
    PH_OW, PH_OY, PH_UH, PH_UW,
    /* Nasals */
    PH_M,  PH_N,  PH_NG,
    /* Voiced stops */
    PH_B,  PH_D,  PH_G,
    /* Voiceless stops */
    PH_P,  PH_T,  PH_K,
    /* Voiced fricatives */
    PH_V,  PH_DH, PH_Z,  PH_ZH,
    /* Voiceless fricatives */
    PH_F,  PH_TH, PH_S,  PH_SH, PH_HH,
    /* Affricates */
    PH_CH, PH_JH,
    /* Approximants */
    PH_L,  PH_R,  PH_W,  PH_Y,
    /* Silence */
    PH_SIL,
    PH_COUNT
} Phoneme;

/* Phoneme class flags */
#define CL_VOWEL    0x01
#define CL_NASAL    0x02
#define CL_STOP     0x04
#define CL_FRIC     0x08
#define CL_VOICED   0x10
#define CL_APPROX   0x20
#define CL_AFFRIC   0x40
#define CL_SIL      0x80

typedef struct {
    const char *name;
    int         cls;
    double      dur_ms;     /* typical duration */
    double      f[NUM_FORMANTS]; /* centre freqs Hz   */
    double      bw[NUM_FORMANTS];/* bandwidths   Hz   */
    double      noise_freq; /* for fricatives: noise LP cutoff */
    double      noise_amp;  /* fraction of noise vs buzz       */
    double      amp;        /* overall amplitude scale         */
} PhonDef;

/* Formant data loosely based on Peterson & Barney (1952) + Klatt (1980) */
static const PhonDef PHDEF[PH_COUNT] = {
/* name class dur   F1   F2   F3   F4     BW1 BW2 BW3 BW4   nfreq namp amp */

/* ─── VOWELS (DECtalk‑style) ───────────────────────────────────────────── */
[PH_AA]={"AA",CL_VOWEL|CL_VOICED,160,{750,1100,2500,3300},{60,80,110,180},0,0.0,1.0},
[PH_AE]={"AE",CL_VOWEL|CL_VOICED,180,{650,1700,2400,3300},{60,80,110,180},0,0.0,1.0},
[PH_AH]={"AH",CL_VOWEL|CL_VOICED,140,{600,1100,2400,3300},{60,80,110,180},0,0.0,0.9},
[PH_AO]={"AO",CL_VOWEL|CL_VOICED,180,{500,900,2400,3300},{60,80,110,180},0,0.0,1.0},
[PH_AW]={"AW",CL_VOWEL|CL_VOICED,200,{650,1000,2500,3300},{60,80,110,180},0,0.0,1.0},
[PH_AY]={"AY",CL_VOWEL|CL_VOICED,200,{600,1800,2600,3300},{60,80,110,180},0,0.0,1.0},
[PH_EH]={"EH",CL_VOWEL|CL_VOICED,160,{550,1800,2500,3300},{60,80,110,180},0,0.0,1.0},
[PH_ER]={"ER",CL_VOWEL|CL_VOICED,180,{450,1350,1700,3300},{60,80,110,180},0,0.0,0.9},
[PH_EY]={"EY",CL_VOWEL|CL_VOICED,180,{400,2000,2800,3300},{60,80,110,180},0,0.0,1.0},
[PH_IH]={"IH",CL_VOWEL|CL_VOICED,130,{350,1900,2500,3300},{60,80,110,180},0,0.0,0.9},
[PH_IY]={"IY",CL_VOWEL|CL_VOICED,160,{250,2300,3000,3600},{50,70,100,150},0,0.0,1.0},
[PH_OW]={"OW",CL_VOWEL|CL_VOICED,180,{450,800,2400,3300},{60,80,110,180},0,0.0,1.0},
[PH_OY]={"OY",CL_VOWEL|CL_VOICED,200,{450,1000,2400,3300},{60,80,110,180},0,0.0,1.0},
[PH_UH]={"UH",CL_VOWEL|CL_VOICED,130,{400,900,2300,3300},{60,80,110,180},0,0.0,0.9},
[PH_UW]={"UW",CL_VOWEL|CL_VOICED,160,{300,900,2200,3300},{60,80,110,180},0,0.0,1.0},

/* ─── NASALS ───────────────────────────────────────────────────────────── */
[PH_M] ={"M", CL_NASAL|CL_VOICED,80,{300,900,2200,3300},{50,70,100,150},0,0.0,0.5},
[PH_N] ={"N", CL_NASAL|CL_VOICED,70,{280,1700,2600,3300},{50,70,100,150},0,0.0,0.5},
[PH_NG]={"NG",CL_NASAL|CL_VOICED,80,{280,900,2300,3300},{50,70,100,150},0,0.0,0.4},

/* ─── VOICED STOPS ─────────────────────────────────────────────────────── */
[PH_B] ={"B",CL_STOP|CL_VOICED,70,{200,900,2200,3300},{80,100,130,200},1000,0.05,0.3},
[PH_D] ={"D",CL_STOP|CL_VOICED,60,{200,1700,2600,3300},{80,100,130,200},1500,0.05,0.3},
[PH_G] ={"G",CL_STOP|CL_VOICED,70,{200,900,2300,3300},{80,100,130,200},1500,0.05,0.3},

/* ─── VOICELESS STOPS ─────────────────────────────────────────────────── */
[PH_P] ={"P",CL_STOP,70,{200,800,2200,3300},{80,100,130,200},2000,0.6,0.3},
[PH_T] ={"T",CL_STOP,60,{200,1700,2600,3300},{80,100,130,200},4000,0.6,0.3},
[PH_K] ={"K",CL_STOP,70,{200,900,2300,3300},{80,100,130,200},3000,0.6,0.3},

/* ─── VOICED FRICATIVES ───────────────────────────────────────────────── */
[PH_V] ={"V",CL_FRIC|CL_VOICED,90,{300,900,2200,3300},{80,100,130,200},4000,0.4,0.5},
[PH_DH]={"DH",CL_FRIC|CL_VOICED,80,{300,1700,2600,3300},{80,100,130,200},3000,0.4,0.4},
[PH_Z] ={"Z",CL_FRIC|CL_VOICED,90,{300,1700,2600,3300},{80,100,130,200},5000,0.5,0.5},
[PH_ZH]={"ZH",CL_FRIC|CL_VOICED,90,{300,1700,2600,3300},{80,100,130,200},4000,0.5,0.5},

/* ─── VOICELESS FRICATIVES ───────────────────────────────────────────── */
[PH_F] ={"F",CL_FRIC,90,{300,900,2200,3300},{80,100,130,200},4000,0.9,0.4},
[PH_TH]={"TH",CL_FRIC,90,{300,1700,2600,3300},{80,100,130,200},3500,0.9,0.3},
[PH_S] ={"S",CL_FRIC,90,{300,1700,2600,3300},{80,100,130,200},6000,0.95,0.5},
[PH_SH]={"SH",CL_FRIC,90,{300,1700,2500,3300},{80,100,130,200},4500,0.95,0.5},
[PH_HH]={"HH",CL_FRIC|CL_VOICED,70,{500,1400,2500,3300},{80,120,150,200},3000,0.4,0.4},

/* ─── AFFRICATES ───────────────────────────────────────────────────────── */
[PH_CH]={"CH",CL_AFFRIC,110,{300,1700,2500,3300},{80,100,130,200},4500,0.8,0.4},
[PH_JH]={"JH",CL_AFFRIC|CL_VOICED,110,{300,1700,2500,3300},{80,100,130,200},4000,0.7,0.4},

/* ─── APPROXIMANTS ───────────────────────────────────────────────────── */
[PH_L] ={"L",CL_APPROX|CL_VOICED,70,{380,1000,2750,3300},{70,90,120,180},0,0.0,0.7},
[PH_R] ={"R",CL_APPROX|CL_VOICED,70,{450,1100,1600,3300},{70,90,120,180},0,0.0,0.7},
[PH_W] ={"W",CL_APPROX|CL_VOICED,70,{300,600,2200,3300},{70,90,120,180},0,0.0,0.7},
[PH_Y] ={"Y",CL_APPROX|CL_VOICED,60,{270,2100,2900,3300},{60,80,110,180},0,0.0,0.7},

/* ─── SILENCE ─────────────────────────────────────────────────────────── */
[PH_SIL]={"SIL",CL_SIL,80,{0,0,0,0},{0,0,0,0},0,0.0,0.0},
};


/* ═══════════════════════════════════════════════════════════════════════════
 * G2P: Grapheme-to-Phoneme
 *
 * Rule table: each entry is a left-context, trigger-letters, right-context
 * → phoneme sequence. Processed left-to-right, longest match wins.
 * This is a simplified but functional rule set for English.
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    const char *match;    /* letters to match (may be multi-char digraph) */
    const char *phones;   /* space-separated phoneme names, "" = silent    */
} G2PRule;

/* Simple flat rule table – ordered longest-match first within each letter */
static const G2PRule G2P_RULES[] = {
    /* ── digraphs / trigraphs first ─────────────────────────────────── */
    {"tch",  "CH"},
    {"sch",  "SH"},
    {"dge",  "JH"},
    {"igh",  "AY"},
    {"ght",  "T"},
    {"ghn",  "N"},
    {"gue",  "G"},
    {"que",  "K"},
    {"ck",   "K"},
    {"ch",   "CH"},
    {"sh",   "SH"},
    {"th",   "DH"},   /* default voiced; voiceless handled below */
    {"ng",   "NG"},
    {"nk",   "NG K"},
    {"ph",   "F"},
    {"wh",   "W"},
    {"wr",   "R"},
    {"kn",   "N"},
    {"gn",   "N"},
    {"mb",   "M"},
    {"mn",   "M"},
    {"rh",   "R"},
    {"ps",   "S"},
    {"tion", "SH AH N"},
    {"sion", "ZH AH N"},
    {"ture", "CH ER"},
    {"dure", "JH ER"},
    {"ous",  "AH S"},
    {"ious", "IY AH S"},
    {"eous", "IY AH S"},
    {"age",  "AH JH"},
    {"ace",  "EY S"},
    {"ice",  "AY S"},
    {"uce",  "UW S"},
    {"ance", "AH N S"},
    {"ence", "AH N S"},
    {"ible", "AH B AH L"},
    {"able", "EY B AH L"},
    {"tion", "SH AH N"},
    /* ── vowel digraphs ───────────────────────────────────────────────── */
    {"ai",   "EY"},
    {"ay",   "EY"},
    {"au",   "AO"},
    {"aw",   "AO"},
    {"ae",   "AE"},
    {"ea",   "IY"},
    {"ee",   "IY"},
    {"ei",   "EY"},
    {"ey",   "EY"},
    {"ie",   "IY"},
    {"eu",   "UW"},
    {"ew",   "UW"},
    {"oa",   "OW"},
    {"oe",   "OW"},
    {"oi",   "OY"},
    {"oo",   "UW"},
    {"ou",   "AW"},
    {"ow",   "OW"},
    {"oy",   "OY"},
    {"ue",   "UW"},
    {"ui",   "UW"},
    /* ── single vowels + context ─────────────────────────────────────── */
    {"a",    "AE"},
    {"e",    "EH"},
    {"i",    "IH"},
    {"o",    "AO"},
    {"u",    "AH"},
    {"y",    "IH"},
    /* ── consonants ──────────────────────────────────────────────────── */
    {"b",    "B"},
    {"c",    "K"},
    {"d",    "D"},
    {"f",    "F"},
    {"g",    "G"},
    {"h",    "HH"},
    {"j",    "JH"},
    {"k",    "K"},
    {"l",    "L"},
    {"m",    "M"},
    {"n",    "N"},
    {"p",    "P"},
    {"q",    "K"},
    {"r",    "R"},
    {"s",    "S"},
    {"t",    "T"},
    {"v",    "V"},
    {"w",    "W"},
    {"x",    "K S"},
    {"z",    "Z"},
    {NULL,   NULL}
};

/* Known word dictionary (highest priority over rules) */
typedef struct { const char *word; const char *phones; } DictEntry;
static const DictEntry DICT[] = {
    {"the",    "DH AH"},
    {"a",      "AH"},
    {"an",     "AE N"},
    {"and",    "AE N D"},
    {"or",     "AO R"},
    {"of",     "AH V"},
    {"to",     "T UW"},
    {"in",     "IH N"},
    {"is",     "IH Z"},
    {"it",     "IH T"},
    {"are",    "AA R"},
    {"was",    "W AH Z"},
    {"be",     "B IY"},
    {"been",   "B IH N"},
    {"have",   "HH AE V"},
    {"has",    "HH AE Z"},
    {"had",    "HH AE D"},
    {"do",     "D UW"},
    {"does",   "D AH Z"},
    {"did",    "D IH D"},
    {"not",    "N AO T"},
    {"no",     "N OW"},
    {"i",      "AY"},
    {"he",     "HH IY"},
    {"she",    "SH IY"},
    {"we",     "W IY"},
    {"you",    "Y UW"},
    {"they",   "DH EY"},
    {"my",     "M AY"},
    {"your",   "Y AO R"},
    {"his",    "HH IH Z"},
    {"her",    "HH ER"},
    {"our",    "AW ER"},
    {"their",  "DH EH R"},
    {"this",   "DH IH S"},
    {"that",   "DH AE T"},
    {"with",   "W IH TH"},
    {"for",    "F AO R"},
    {"from",   "F R AH M"},
    {"by",     "B AY"},
    {"at",     "AE T"},
    {"as",     "AE Z"},
    {"on",     "AO N"},
    {"up",     "AH P"},
    {"out",    "AW T"},
    {"so",     "S OW"},
    {"if",     "IH F"},
    {"but",    "B AH T"},
    {"about",  "AH B AW T"},
    {"all",    "AO L"},
    {"can",    "K AE N"},
    {"could",  "K UH D"},
    {"would",  "W UH D"},
    {"should",  "SH UH D"},
    {"will",   "W IH L"},
    {"just",   "JH AH S T"},
    {"like",   "L AY K"},
    {"know",   "N OW"},
    {"what",   "W AH T"},
    {"when",   "W EH N"},
    {"where",  "W EH R"},
    {"who",    "HH UW"},
    {"how",    "HH AW"},
    {"why",    "W AY"},
    {"more",   "M AO R"},
    {"very",   "V EH R IY"},
    {"also",   "AO L S OW"},
    {"hello",  "HH EH L OW"},
    {"dr",     "D AO K T ER"},
    {"doctor", "D AO K T ER"},
    {"mr",     "M IH S T ER"},
    {"ms",     "M IH Z"},
    {"mrs",    "M IH S IH Z"},
    {"joshua", "JH AO SH UW AH"},
    {"falken", "F AO L K AH N"},
    {"good",   "G UH D"},
    {"great",  "G R EY T"},
    {"fine",   "F AY N"},
    {"yes",    "Y EH S"},
    {"one",    "W AH N"},
    {"two",    "T UW"},
    {"three",  "TH R IY"},
    {"ok",     "OW K EY"},
    {"okay",   "OW K EY"},
    {"please", "P L IY Z"},
    {"thank",  "TH AE NG K"},
    {"thanks", "TH AE NG K S"},
    {"day",    "D EY"},
    {"time",   "T AY M"},
    {"year",   "Y IH R"},
    {"come",   "K AH M"},
    {"go",     "G OW"},
    {"see",    "S IY"},
    {"look",   "L UH K"},
    {"think",  "TH IH NG K"},
    {"make",   "M EY K"},
    {"say",    "S EY"},
    {"tell",   "T EH L"},
    {"find",   "F AY N D"},
    {"give",   "G IH V"},
    {"take",   "T EY K"},
    {"want",   "W AO N T"},
    {"need",   "N IY D"},
    {"use",    "Y UW Z"},
    {"name",   "N EY M"},
    {"work",   "W ER K"},
    {"call",   "K AO L"},
    {"try",    "T R AY"},
    {"ask",    "AE S K"},
    {"seem",   "S IY M"},
    {"feel",   "F IY L"},
    {"leave",  "L IY V"},
    {"put",    "P UH T"},
    {"mean",   "M IY N"},
    {"keep",   "K IY P"},
    {"let",    "L EH T"},
    {"begin",  "B IH G IH N"},
    {"show",   "SH OW"},
    {"hear",   "HH IY R"},
    {"play",   "P L EY"},
    {"run",    "R AH N"},
    {"move",   "M UW V"},
    {"live",   "L IH V"},
    {"believe","B IH L IY V"},
    {"hold",   "HH OW L D"},
    {"bring",  "B R IH NG"},
    {"happen", "HH AE P AH N"},
    {"write",  "R AY T"},
    {"provide","P R AH V AY D"},
    {"sit",    "S IH T"},
    {"stand",  "S T AE N D"},
    {"lose",   "L UW Z"},
    {"pay",    "P EY"},
    {"meet",   "M IY T"},
    {"include","IH N K L UW D"},
    {"continue","K AH N T IH N Y UW"},
    {"set",    "S EH T"},
    {"learn",  "L ER N"},
    {"change", "CH EY N JH"},
    {"lead",   "L IY D"},
    {"war",    "W AO R"},
    {"game",   "G EY M"},
    {"global",  "G L OW B AH L"},
    {"thermonuclear","TH ER M OW N UW K L IY ER"},
    {"chess",   "CH EH S"},
    {"strange", "S T R EY N JH"},
    {NULL,      NULL}
};

static unsigned hash_word(const char *s) {
    unsigned h = 5381;
    while (*s) h = ((h << 5) + h) ^ tolower(*s++);
    return h % DICT_BUCKETS;
}

static void dict_insert(const char *word, const char *phones) {
    unsigned h = hash_word(word);
    DictNode *n = malloc(sizeof(DictNode));
    n->word = strdup(word);
    n->phones = strdup(phones);
    n->next = cmu_dict[h];
    cmu_dict[h] = n;
}

static void strip_stress(char *p) {
    for (char *s = p; *s; s++)
        if (s[1] == '0' || s[1] == '1' || s[1] == '2')
            s[1] = '\0';
}

/* ── Look up a lowercase word in the dictionary ─────────────────────── */
static const char *dict_lookup(const char *word) {
    unsigned h = hash_word(word);
    for (DictNode *n = cmu_dict[h]; n; n = n->next)
        if (strcmp(n->word, word) == 0)
            return n->phones;
    return NULL;
}

static void load_cmudict(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Could not open CMUdict file: %s\n", path);
        return;
    }

    char line[512];

    while (fgets(line, sizeof(line), f)) {
        /* Skip comments */
        if (line[0] == ';')
            continue;

        /* Extract word */
        char *word = strtok(line, " \t\r\n");
        if (!word)
            continue;

        /* Extract phoneme string */
        char *phones = strtok(NULL, "\r\n");
        if (!phones)
            continue;

        /* Strip alternate pronunciation markers: WORD(1) → WORD */
        char *paren = strchr(word, '(');
        if (paren)
            *paren = '\0';

        /* Lowercase the word */
        for (char *s = word; *s; s++)
            *s = tolower((unsigned char)*s);

        /* Build cleaned phoneme string */
        char buf[512];
        buf[0] = '\0';

        char *tok = strtok(phones, " ");
        while (tok) {
            /* Strip stress digits: AH0 → AH */
            strip_stress(tok);

            /* Append to buffer */
            strcat(buf, tok);
            strcat(buf, " ");

            tok = strtok(NULL, " ");
        }

        /* Insert into dictionary */
        dict_insert(word, buf);
    }

    fclose(f);
}



/* ── Map phoneme name → enum ────────────────────────────────────────── */
static Phoneme name_to_ph(const char *s) {
    for (int i = 0; i < PH_COUNT; i++)
        if (strcmp(PHDEF[i].name, s) == 0) return (Phoneme)i;
    return PH_SIL;
}

/* ── Apply G2P rules to a single lowercase word ─────────────────────── */
static int g2p_word(const char *word, Phoneme *out, int maxout) {
    int n = 0;
    const char *p = word;
    while (*p && n < maxout - 4) {
        /* Try rules longest-match first */
        int matched = 0;
        for (int r = 0; G2P_RULES[r].match; r++) {
            int ml = strlen(G2P_RULES[r].match);
            if (strncmp(p, G2P_RULES[r].match, ml) == 0) {
                /* Parse phones */
                char buf[64]; strncpy(buf, G2P_RULES[r].phones, 63); buf[63]=0;
                char *tok = strtok(buf, " ");
                while (tok && n < maxout-2) {
                    out[n++] = name_to_ph(tok);
                    tok = strtok(NULL, " ");
                }
                p += ml;
                matched = 1;
                break;
            }
        }
        if (!matched) p++; /* skip unknown char */
    }
    return n;
}

/* ── Tokenize text and convert to phoneme sequence ──────────────────── */
static int text_to_phonemes(const char *text, Phoneme *out, int maxout) {
    int n = 0;
    char word[MAX_WORD];

    /* Make a lowercase copy of the input */
    char lower[1024];
    int i = 0;
    while (text[i] && i < (int)sizeof(lower)-1) {
        lower[i] = tolower((unsigned char)text[i]);
        i++;
    }
    lower[i] = '\0';

    const char *p = lower;

    /* Leading silence */
    if (n < maxout) out[n++] = PH_SIL;

    while (*p) {
        /* Skip non-alpha characters */
        while (*p && !isalpha((unsigned char)*p)) {
            if (*p == ',' || *p == ';' || *p == ':') {
                if (n < maxout) out[n++] = PH_SIL;
            } else if (*p == '.' || *p == '!' || *p == '?') {
                if (n < maxout) out[n++] = PH_SIL;
                if (n < maxout) out[n++] = PH_SIL;
            }
            p++;
        }
        if (!*p) break;

        /* Collect a word */
        int wl = 0;
        while (*p && isalpha((unsigned char)*p) && wl < MAX_WORD-1)
            word[wl++] = *p++;
        word[wl] = '\0';

        if (!wl) continue;

        /* Try CMUdict first */
        const char *dict = dict_lookup(word);
        if (dict) {
            printf("[DICT]  %-20s → %s\n", word, dict);

            char buf[256];
            strncpy(buf, dict, 255);
            buf[255] = 0;

            char *tok = strtok(buf, " ");
            while (tok && n < maxout-2) {
                out[n++] = name_to_ph(tok);
                tok = strtok(NULL, " ");
            }
        } else {
            /* Fallback to G2P rules */
            int before = n;
            int added = g2p_word(word, out+n, maxout-n);

            if (added > 0) {
                printf("[G2P ]  %-20s → %d phonemes\n", word, added);
                n += added;
            } else {
                printf("[MISS] %-20s → (no match)\n", word);
            }
        }

        /* Word boundary silence */
        if (n < maxout) out[n++] = PH_SIL;
    }

    /* Trailing silence */
    if (n < maxout) out[n++] = PH_SIL;

    return n;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Synthesizer engine
 * ═══════════════════════════════════════════════════════════════════════════ */

/* One active phoneme segment being rendered */
typedef struct {
    Phoneme ph;
    int     total_samples;
    int     samples_done;
    /* Formant targets for this segment */
    double  tgt_f[NUM_FORMANTS];
    double  tgt_bw[NUM_FORMANTS];
} Segment;

#define MAX_SEG 4   /* ring buffer of upcoming segments for smooth lookahead */

typedef struct {
    /* Audio sources */
    double  phase;      /* glottal saw phase 0..1           */
    double  pitch_hz;

    /* Noise source state (LCG) */
    unsigned noise_seed;
    Biquad  noise_filt;

    /* Formant filters */
    Biquad  filt[NUM_FORMANTS];
    double  cur_f[NUM_FORMANTS];
    double  cur_bw[NUM_FORMANTS];

    /* Phoneme sequencer */
    Phoneme seq[MAX_PHONEMES];
    int     seq_len;
    int     seq_pos;     /* next phoneme to schedule */

    Segment seg;         /* currently rendering segment */
    int     seg_active;

    /* Global envelope */
    double  master_amp;  /* 0..1 */
    int     speaking;    /* 1 while sequence in progress */
} Synth;

static Synth    g_synth;
static SDL_mutex *g_mutex;

/* Fast white noise */
static inline double white_noise(Synth *s) {
    s->noise_seed = s->noise_seed * 1664525u + 1013904223u;
    return (double)(int)s->noise_seed / 2147483648.0;
}

/* ── Advance to next phoneme segment ─────────────────────────────────── */
static void next_segment(Synth *s) {
    if (s->seq_pos >= s->seq_len) {
        s->seg_active = 0;
        s->speaking   = 0;
        return;
    }

    Phoneme ph = s->seq[s->seq_pos++];
    const PhonDef *def = &PHDEF[ph];

    s->seg.ph = ph;

    /* Hawking vowels are slightly elongated */
    double dur = def->dur_ms;
    if (def->cls & CL_VOWEL)
        dur *= 1.15;

    s->seg.total_samples = (int)(dur * 0.001 * SAMPLE_RATE);
    if (s->seg.total_samples < 1)
        s->seg.total_samples = 1;

    s->seg.samples_done = 0;

    for (int f = 0; f < NUM_FORMANTS; f++) {
        s->seg.tgt_f[f]  = def->f[f];
        s->seg.tgt_bw[f] = def->bw[f] * 0.55;  /* DECtalk narrow bandwidths */
    }

    s->seg_active = 1;
}

static void synth_speak(Synth *s, const char *text) {
    s->seq_len = text_to_phonemes(text, s->seq, MAX_PHONEMES);
    s->seq_pos = 0;
    s->seg_active = 0;
    s->speaking   = 1;
    next_segment(s);
}

/* ── Audio callback ───────────────────────────────────────────────────── */
static void audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    Sint16 *out     = (Sint16 *)stream;
    int     samples = len / sizeof(Sint16);

    SDL_LockMutex(g_mutex);
    Synth *s = &g_synth;

    for (int i = 0; i < samples; i++) {
        double sample = 0.0;

        if (!s->seg_active && s->speaking)
            next_segment(s);

        if (s->seg_active) {
            const PhonDef *def = &PHDEF[s->seg.ph];
            double t = (double)s->seg.samples_done / s->seg.total_samples;

            /* Hawking amplitude envelope: very flat */
            double env;
            if      (t < 0.05) env = t / 0.05;
            else if (t > 0.95) env = (1.0 - t) / 0.05;
            else               env = 1.0;
            env *= def->amp;

            /* DECtalk-style fast formant transitions */
            double smooth = 0.004;

            for (int f = 0; f < NUM_FORMANTS; f++) {
                s->cur_f[f]  += smooth * (s->seg.tgt_f[f]  - s->cur_f[f]);
                s->cur_bw[f] += smooth * (s->seg.tgt_bw[f] - s->cur_bw[f]);

                /* Narrow bandwidths = metallic Hawking timbre */
                double bw = s->cur_bw[f] * 0.55;

                if ((i & 31) == 0 && s->cur_f[f] > 1.0)
                    biquad_bp(&s->filt[f], s->cur_f[f], bw);
            }

            /* Hawking pitch: flat, monotone */
            s->pitch_hz = 87.0;

            /* DECtalk glottal source (clipped saw) */
            s->phase += s->pitch_hz / SAMPLE_RATE;
            if (s->phase >= 1.0) s->phase -= 1.0;

            double x = 2.0 * s->phase - 1.0;
            double buzz = x - 0.3 * x * x * x;

            /* Very little noise in Hawking vowels */
            double noise_amp = def->noise_amp * 0.25;
            double voiced_amp = 1.0;

            double noise = white_noise(s);
            if (def->noise_freq > 0 && (i & 15) == 0)
                biquad_lp(&s->noise_filt, def->noise_freq);
            noise = bq(&s->noise_filt, noise) * noise_amp;

            double src;

            if (def->cls & CL_STOP) {
                double burst_start = 0.7;
                if (t < burst_start)
                    src = (def->cls & CL_VOICED) ? buzz * 0.05 : 0.0;
                else
                    src = noise * 3.0 + ((def->cls & CL_VOICED) ? buzz * 0.3 : 0.0);
            } else if (def->cls & CL_SIL) {
                src = 0.0;
            } else {
                src = buzz * voiced_amp + noise;
            }

            double filtered = 0.0;
            for (int f = 0; f < NUM_FORMANTS; f++)
                if (s->cur_f[f] > 1.0)
                    filtered += bq(&s->filt[f], src);

            /* Metallic resonance boost */
            filtered *= 1.15;

            sample = tanh(filtered * 1.2) * env;

            s->seg.samples_done++;
            if (s->seg.samples_done >= s->seg.total_samples) {
                s->seg_active = 0;
                if (s->seq_pos < s->seq_len)
                    next_segment(s);
                else
                    s->speaking = 0;
            }
        }

        /* Master amplitude */
        if (s->speaking)
            s->master_amp += (1.0 - s->master_amp) * 0.001;
        else
            s->master_amp += (0.0 - s->master_amp) * 0.002;

        *out++ = (Sint16)(sample * s->master_amp * 26000.0);
    }

    SDL_UnlockMutex(g_mutex);
}


/* ── Initialise synthesizer ─────────────────────────────────────────── */
static void synth_init(Synth *s) {
    memset(s, 0, sizeof(*s));
    s->pitch_hz   = 120.0;
    s->noise_seed = 12345;
    /* Prime formants at AH */
    const PhonDef *def = &PHDEF[PH_AH];
    for (int f = 0; f < NUM_FORMANTS; f++) {
        s->cur_f[f]  = def->f[f]  > 0 ? def->f[f]  : 500.0;
        s->cur_bw[f] = def->bw[f] > 0 ? def->bw[f] : 80.0;
        biquad_bp(&s->filt[f], s->cur_f[f], s->cur_bw[f]);
    }
    biquad_lp(&s->noise_filt, 4000.0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    g_mutex = SDL_CreateMutex();
    synth_init(&g_synth);

    SDL_AudioSpec want = {0}, got;
    want.freq     = SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = BUFFER_SIZE;
    want.callback = audio_callback;

    printf("Loading Dictionary\n");
    load_cmudict("cmudict-0.7b");
    printf("End Loading Dictionary\n");


    if (SDL_OpenAudio(&want, &got) < 0) {
        fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
        return 1;
    }

    SDL_PauseAudio(0);

    /* Interactive mode if no argument */
    if (argc < 2) {
        char line[1024];
        printf("Formant TTS ready. Type text and press Enter. Q to quit.\n");

        while (1) {
            printf("> ");
            fflush(stdout);

            if (!fgets(line, sizeof(line), stdin))
                break;

            if (line[0] == 'q' || line[0] == 'Q')
                break;

            SDL_LockMutex(g_mutex);
            synth_speak(&g_synth, line);
            SDL_UnlockMutex(g_mutex);
        }
    } else {
        /* Speak command-line argument */
        SDL_LockMutex(g_mutex);
        synth_speak(&g_synth, argv[1]);
        SDL_UnlockMutex(g_mutex);

        /* Wait until done */
        while (1) {
            SDL_LockMutex(g_mutex);
            int speaking = g_synth.speaking;
            SDL_UnlockMutex(g_mutex);
            if (!speaking) break;
            SDL_Delay(20);
        }
    }

    SDL_CloseAudio();
    SDL_DestroyMutex(g_mutex);
    SDL_Quit();
    return 0;
}

