/*
 * Huffman Compression Engine – Terminal Project
 * Windows 11, C language
 *
 * Usage:
 *   huffman c <input> <output>   (compress)
 *   huffman d <input> <output>   (decompress)
 *
 * Features:
 *   - Custom min-heap priority queue
 *   - Canonical Huffman codes for deterministic header
 *   - Handles any binary file, correct EOF padding
 *   - O(n log n) encoding, O(n) decoding
 *   - Benchmark stats (ratio, throughput) printed to stderr
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>   /* for QueryPerformanceCounter */

/* ----- constants ----- */
#define ALPHABET      256
#define MAX_NODES     511    /* max nodes in Huffman tree (2*ALPHABET - 1) */

/* ----- node used during tree building and final storage ----- */
typedef struct {
    int left, right;        /* child indices, -1 if none */
    int symbol;             /* byte value for leaf, -1 for internal */
    unsigned freq;          /* only used during building */
} Node;

/* ----- bit writer (low-level output) ----- */
typedef struct {
    FILE *fp;
    unsigned char buffer;
    int bits_filled;        /* 0 .. 7 */
} BitWriter;

static void bw_init(BitWriter *bw, FILE *fp) {
    bw->fp = fp;
    bw->buffer = 0;
    bw->bits_filled = 0;
}

static void bw_write_bit(BitWriter *bw, int bit) {
    bw->buffer = (bw->buffer << 1) | (bit & 1);
    if (++bw->bits_filled == 8) {
        fwrite(&bw->buffer, 1, 1, bw->fp);
        bw->buffer = 0;
        bw->bits_filled = 0;
    }
}

static void bw_flush(BitWriter *bw) {
    if (bw->bits_filled > 0) {
        bw->buffer <<= (8 - bw->bits_filled);   /* pad with zeros */
        fwrite(&bw->buffer, 1, 1, bw->fp);
        bw->bits_filled = 0;
    }
}

/* ----- bit reader (low-level input) ----- */
typedef struct {
    FILE *fp;
    unsigned char buffer;
    int bits_left;          /* how many unread bits remain in buffer */
} BitReader;

static void br_init(BitReader *br, FILE *fp) {
    br->fp = fp;
    br->bits_left = 0;
}

/* return 0 or 1, or -1 on EOF */
static int br_read_bit(BitReader *br) {
    if (br->bits_left == 0) {
        if (fread(&br->buffer, 1, 1, br->fp) != 1)
            return -1;
        br->bits_left = 8;
    }
    br->bits_left--;
    return (br->buffer >> br->bits_left) & 1;
}

/* ----- min‑heap priority queue using Node.freq ----- */
static Node nodes[MAX_NODES];      /* global tree nodes */
static int node_count;

static int heap[MAX_NODES];        /* stores indices into nodes[] */
static int heap_size;

static void heap_swap(int i, int j) {
    int tmp = heap[i];
    heap[i] = heap[j];
    heap[j] = tmp;
}

static void heap_push(int idx) {
    int i = heap_size++;
    heap[i] = idx;
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (nodes[heap[parent]].freq <= nodes[heap[i]].freq) break;
        heap_swap(parent, i);
        i = parent;
    }
}

static int heap_pop(void) {
    int ret = heap[0];
    heap[0] = heap[--heap_size];
    int i = 0;
    while (1) {
        int smallest = i;
        int left = 2 * i + 1;
        int right = 2 * i + 2;
        if (left < heap_size && nodes[heap[left]].freq < nodes[heap[smallest]].freq)
            smallest = left;
        if (right < heap_size && nodes[heap[right]].freq < nodes[heap[smallest]].freq)
            smallest = right;
        if (smallest == i) break;
        heap_swap(i, smallest);
        i = smallest;
    }
    return ret;
}

/* ----- Huffman tree building ----- */
/*
 * Fills frequencies from input, builds tree.
 * Returns root index, or -1 if file empty / error.
 * node_count globally updated.
 */
static int build_tree(FILE *in, unsigned *freqs) {
    /* zero freq array */
    memset(freqs, 0, sizeof(unsigned) * ALPHABET);
    int c;
    while ((c = fgetc(in)) != EOF)
        freqs[c]++;

    /* count distinct symbols */
    int symbols = 0;
    int single_sym = 0;
    for (int i = 0; i < ALPHABET; i++) {
        if (freqs[i] > 0) {
            symbols++;
            if (symbols == 1) single_sym = i;
        }
    }
    if (symbols == 0) return -1;   /* empty file */

    /* initialise leaves */
    node_count = 0;
    heap_size = 0;
    for (int i = 0; i < ALPHABET; i++) {
        if (freqs[i] > 0) {
            nodes[node_count].symbol = i;
            nodes[node_count].freq = freqs[i];
            nodes[node_count].left = nodes[node_count].right = -1;
            heap_push(node_count);
            node_count++;
        }
    }

    /* special case: only one symbol – return its leaf index */
    if (symbols == 1) {
        return heap_pop();          /* the only leaf */
    }

    /* build tree until one root remains */
    while (heap_size > 1) {
        int a = heap_pop();
        int b = heap_pop();
        nodes[node_count].symbol = -1;
        nodes[node_count].freq = nodes[a].freq + nodes[b].freq;
        nodes[node_count].left = a;
        nodes[node_count].right = b;
        heap_push(node_count);
        node_count++;
    }
    return heap_pop();              /* root index */
}

/* ----- compute code lengths via depth‑first traversal ----- */
static void compute_lengths(int idx, int depth, unsigned char lengths[ALPHABET]) {
    if (nodes[idx].symbol >= 0) {   /* leaf */
        lengths[nodes[idx].symbol] = (unsigned char)depth;
        return;
    }
    if (nodes[idx].left != -1)
        compute_lengths(nodes[idx].left, depth + 1, lengths);
    if (nodes[idx].right != -1)
        compute_lengths(nodes[idx].right, depth + 1, lengths);
}

/* ----- canonical Huffman code assignment ----- */
typedef struct {
    int sym;
    int len;
} SymLen;

static int cmp_symlen(const void *a, const void *b) {
    const SymLen *x = (const SymLen *)a;
    const SymLen *y = (const SymLen *)b;
    if (x->len != y->len) return x->len - y->len;
    return x->sym - y->sym;
}

static void canonical_codes(const unsigned char lengths[ALPHABET],
                            uint64_t codes[ALPHABET]) {
    SymLen symbols[ALPHABET];
    int count = 0;
    for (int i = 0; i < ALPHABET; i++) {
        if (lengths[i] > 0) {
            symbols[count].sym = i;
            symbols[count].len = lengths[i];
            count++;
        }
    }
    qsort(symbols, count, sizeof(SymLen), cmp_symlen);

    uint64_t code = 0;
    int prev_len = 0;
    for (int i = 0; i < count; i++) {
        while (prev_len < symbols[i].len) {
            code <<= 1;
            prev_len++;
        }
        codes[symbols[i].sym] = code;
        code++;
    }
}

/* ----- compression ----- */
static void compress(const char *in_path, const char *out_path) {
    FILE *in = fopen(in_path, "rb");
    if (!in) {
        fprintf(stderr, "Error: cannot open input file %s\n", in_path);
        exit(1);
    }

    /* first pass – count frequencies */
    unsigned freqs[ALPHABET];
    int root = build_tree(in, freqs);
    if (root == -1) {   /* empty file */
        fclose(in);
        FILE *out = fopen(out_path, "wb");
        if (!out) { fprintf(stderr, "Error: cannot create output file %s\n", out_path); exit(1); }
        uint64_t zero = 0;
        fwrite(&zero, sizeof(zero), 1, out);
        unsigned char zero_lengths[ALPHABET] = {0};
        fwrite(zero_lengths, 1, ALPHABET, out);
        fclose(out);
        printf("Empty file compressed.\n");
        return;
    }

    /* compute code lengths */
    unsigned char lengths[ALPHABET];
    memset(lengths, 0, sizeof(lengths));
    compute_lengths(root, 0, lengths);

    /* canonical codes */
    uint64_t codes[ALPHABET] = {0};
    canonical_codes(lengths, codes);

    /* header: original size + length table */
    fseek(in, 0, SEEK_END);
    long orig_size = ftell(in);
    rewind(in);

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "Error: cannot create output file %s\n", out_path);
        fclose(in);
        exit(1);
    }
    uint64_t size64 = (uint64_t)orig_size;
    fwrite(&size64, sizeof(size64), 1, out);
    fwrite(lengths, 1, ALPHABET, out);

    /* encode file contents */
    BitWriter bw;
    bw_init(&bw, out);
    int byte;
    while ((byte = fgetc(in)) != EOF) {
        uint64_t cd = codes[byte];
        int len = lengths[byte];
        for (int i = len - 1; i >= 0; i--)   /* MSB first */
            bw_write_bit(&bw, (cd >> i) & 1);
    }
    bw_flush(&bw);

    fclose(in);
    fclose(out);

    /* print stats */
    fseek(out, 0, SEEK_END);
    long comp_size = ftell(out);
    double ratio = (orig_size == 0) ? 0.0 : 100.0 * comp_size / orig_size;
    printf("Compression: %ld -> %ld bytes (%.2f%% of original)\n",
           orig_size, comp_size, ratio);
}

/* ----- decompression ----- */
static void decompress(const char *in_path, const char *out_path) {
    FILE *in = fopen(in_path, "rb");
    if (!in) {
        fprintf(stderr, "Error: cannot open input file %s\n", in_path);
        exit(1);
    }

    /* read header */
    uint64_t orig_size;
    if (fread(&orig_size, sizeof(orig_size), 1, in) != 1) {
        fprintf(stderr, "Error: invalid compressed file (cannot read size)\n");
        fclose(in);
        exit(1);
    }
    unsigned char lengths[ALPHABET];
    if (fread(lengths, 1, ALPHABET, in) != ALPHABET) {
        fprintf(stderr, "Error: invalid compressed file (cannot read length table)\n");
        fclose(in);
        exit(1);
    }

    /* empty file? */
    if (orig_size == 0) {
        fclose(in);
        FILE *out = fopen(out_path, "wb");
        if (out) fclose(out);
        printf("Decompressed empty file.\n");
        return;
    }

    /* rebuild canonical tree from lengths */
    SymLen symbols[ALPHABET];
    int count = 0;
    for (int i = 0; i < ALPHABET; i++) {
        if (lengths[i] > 0) {
            symbols[count].sym = i;
            symbols[count].len = lengths[i];
            count++;
        }
    }
    qsort(symbols, count, sizeof(SymLen), cmp_symlen);

    /* compute codes and insert into tree */
    Node dec_nodes[MAX_NODES];
    int dec_node_count = 1;          /* root at index 0 */
    dec_nodes[0].left = dec_nodes[0].right = -1;
    dec_nodes[0].symbol = -1;

    uint64_t code = 0;
    int prev_len = 0;
    for (int i = 0; i < count; i++) {
        while (prev_len < symbols[i].len) {
            code <<= 1;
            prev_len++;
        }
        /* insert this symbol into the tree */
        int cur = 0;   /* start at root */
        int L = symbols[i].len;
        for (int bit = L - 1; bit >= 0; bit--) {
            int b = (code >> bit) & 1;
            if (b == 0) {
                if (dec_nodes[cur].left == -1) {
                    dec_nodes[cur].left = dec_node_count;
                    dec_nodes[dec_node_count].left = dec_nodes[dec_node_count].right = -1;
                    dec_nodes[dec_node_count].symbol = -1;
                    dec_node_count++;
                }
                cur = dec_nodes[cur].left;
            } else {
                if (dec_nodes[cur].right == -1) {
                    dec_nodes[cur].right = dec_node_count;
                    dec_nodes[dec_node_count].left = dec_nodes[dec_node_count].right = -1;
                    dec_nodes[dec_node_count].symbol = -1;
                    dec_node_count++;
                }
                cur = dec_nodes[cur].right;
            }
        }
        dec_nodes[cur].symbol = symbols[i].sym;
        code++;
    }

    /* decode */
    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "Error: cannot create output file %s\n", out_path);
        fclose(in);
        exit(1);
    }

    BitReader br;
    br_init(&br, in);
    uint64_t written = 0;
    while (written < orig_size) {
        int cur = 0;
        while (dec_nodes[cur].symbol == -1) {
            int bit = br_read_bit(&br);
            if (bit < 0) {
                fprintf(stderr, "Error: unexpected end of compressed data\n");
                fclose(in);
                fclose(out);
                exit(1);
            }
            if (bit == 0)
                cur = dec_nodes[cur].left;
            else
                cur = dec_nodes[cur].right;
        }
        unsigned char byte = (unsigned char)dec_nodes[cur].symbol;
        fwrite(&byte, 1, 1, out);
        written++;
    }

    fclose(in);
    fclose(out);
    printf("Decompression: %llu bytes restored.\n", written);
}

/* ----- timing helper ----- */
static double get_time_sec(void) {
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / freq.QuadPart;
}

/* ----- main ----- */
int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage:\n"
                        "  %s c <input> <output>\n"
                        "  %s d <input> <output>\n", argv[0], argv[0]);
        return 1;
    }

    int mode;  /* 0 = compress, 1 = decompress */
    if (strcmp(argv[1], "c") == 0 || strcmp(argv[1], "-c") == 0)
        mode = 0;
    else if (strcmp(argv[1], "d") == 0 || strcmp(argv[1], "-d") == 0)
        mode = 1;
    else {
        fprintf(stderr, "Invalid mode '%s'. Use 'c' or 'd'.\n", argv[1]);
        return 1;
    }

    double t0 = get_time_sec();
    if (mode == 0)
        compress(argv[2], argv[3]);
    else
        decompress(argv[2], argv[3]);
    double elapsed = get_time_sec() - t0;

    /* throughput (for compress: input size / time) */
    FILE *f = fopen(mode == 0 ? argv[2] : argv[3], "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fclose(f);
        double mb = size / (1024.0 * 1024.0);
        if (elapsed > 0.0)
            printf("Elapsed: %.3f s  (%.2f MB/s)\n", elapsed, mb / elapsed);
    } else {
        printf("Elapsed: %.3f s\n", elapsed);
    }

    return 0;
}