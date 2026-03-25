#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <stdarg.h>

// Simple, dependency-free terminal color picker
// - determines terminal size
// - renders a left grayscale gradient and a right hue gradient in ANSI truecolor
// - allows moving in the gradient with arrow keys
// - a hue line below can be moved with a or d
// - shows color in hex, rgb, cmyk, hsv, hsl
// - supports basic text input editing for the color representations

#define MAX_INPUT 128

// Global terminal state
static struct termios orig_termios;

typedef enum {
    NONE,
    HEX,
    RGB,
    CMYK,
    HSV,
    HSL
} CodeType;

static int term_width = 80;
static int term_height = 24;
static int grad_width = 0;  // width of gradient area
static int grad_height = 0; // height for gradient area
static int sg = 0;          // grayscale width (left half)
static int cg = 0;          // color gradient width (right half)
static int hue_line_width = 0;

static int cursor_x = 0;     // gradient cursor x
static int cursor_y = 0;     // gradient cursor x
static int hue_line_x = 0;
static int hue_line_hue = 0;

static CodeType color_code_type = NONE; // 0 none,1 hex,2 rgb,3 cmyk,4 hsv,5 hsl
static int editing = 0;      // -1 none, else editing the section index
static char input_buf[MAX_INPUT];
static int input_len = 0;

// Current color representation (RGB 0-255) and derived HSV/HSL
static float cur_r = 0, cur_g = 0, cur_b = 0;
static float cur_h = 0.0f, cur_s = 0.0f, cur_v = 0.0f;    // HSV
static float cur_hs = 0.0f, cur_ss = 0.0f, cur_ls = 0.0f; // HSL

// log
#define RED      "\x1b[38;2;255;0;0m"
#define ORANGE   "\x1b[38;2;230;76;0m"
#define YELLOW   "\x1b[38;2;230;226;0m"
#define GREEN    "\x1b[38;2;0;186;40m"
#define BLUE     "\x1b[38;2;0;72;255m"
#define INDIGO   "\x1b[38;2;84;0;230m"
#define VIOLET   "\x1b[38;2;176;0;230m"
#define GREY   "\x1b[38;2;105;105;105m"
#define ANSI_RESET "\x1b[0m"

#define TRACE "TRACE"
#define DEBUG "DEBUG"
#define INFO "INFO"
#define WARN "WARN"
#define ERROR "ERROR"
char LOG_FILE[24];
int log_c = 0;
void write_f(char* file_path, char* str) {
    FILE *file = fopen(file_path, "w");
    if (file) {
        fprintf(file, str);
        fclose(file);
    }
}

void c_log(char* level, char* message) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

    char* color;
    if (strcmp(level, TRACE) == 0) color = YELLOW;
    else if (strcmp(level, DEBUG) == 0) color = GREEN;
    else if (strcmp(level, INFO) == 0) color = BLUE;
    else if (strcmp(level, WARN) == 0) color = ORANGE;
    else if (strcmp(level, ERROR) == 0) color = RED;

    size_t size = strlen(color) + strlen(level) + strlen(time_str) + strlen(message) + 128;

    char *entire_message = malloc(size);
    if (!entire_message)
        return;

    /* Build formatted message */
    // printf(
    //     "%s[%s]%s %s%s%s %s\n",
    //     color,
    //     level,
    //     ANSI_RESET,
    //     GREY,
    //     time_str,
    //     ANSI_RESET,
    //     message
    // );

    /* Write to file (without ANSI colors) */
    FILE *file = fopen(LOG_FILE, "a");
    if (file) {
        fprintf(file, "[%s] %s %s\n", level, time_str, message);
        fclose(file);
    }

    free(entire_message);


}

void c_logf(char* level, char* message, ...) {

    /* Format the message with the provided arguments */
    va_list args1, args2;
    va_start(args1, message);
    va_copy(args2, args1);  // Copy for second use
    
    /* Calculate required buffer size */
    int msg_len = vsnprintf(NULL, 0, message, args1);
    va_end(args1);
    
    if (msg_len < 0) {
        va_end(args2);
        return;
    }
    
    char *formatted_msg = malloc(msg_len + 1);
    if (!formatted_msg) {
        va_end(args2);
        return;
    }
    
    vsnprintf(formatted_msg, msg_len + 1, message, args2);
    va_end(args2);
    
    c_log(level, formatted_msg);
    
    free(formatted_msg);
}


// Utility: clamp
static int clamp_i(int v, int a, int b)
{
    if (v < a)
        return a;
    if (v > b)
        return b;
    return v;
}
static float clamp_f(float v, float a, float b)
{
    if (v < a)
        return a;
    if (v > b)
        return b;
    return v;
}

// Terminal helpers
static void reset_terminal_mode()
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    // show cursor
    printf("\033[?25h");
    fflush(stdout);
}
static void set_raw_mode()
{
    struct termios raw = orig_termios;
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(reset_terminal_mode);

    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    // hide cursor
    printf("\033[?25l");
    fflush(stdout);
}

static void get_terminal_size(int *w, int *h)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0)
    {
        *w = ws.ws_col;
        *h = ws.ws_row;
    }
}

static void clear_screen() { printf("\033[2J"); }
static void move_cursor(int c, int r) {
    fflush(stdout);
    printf("\033[%d;%dH", r+1, c+1); 
}
static void go_to_home() { 
    move_cursor(0,0);
    // printf("\033[H"); 
}
static void hide_cursor() { printf("\033[?25l"); }
static void show_cursor() { printf("\033[?25h"); }

// printing
typedef struct {
    void **data;
    size_t length;
    size_t capacity;
} ptr_array;

static ptr_array* ptr_array_new() {
    ptr_array* a = malloc(sizeof(ptr_array));
    a->data = malloc(sizeof(void *) * 100);
    a->capacity = 100;
    a->length = 0;
    memset(a->data, 0, a->capacity * sizeof(void *));

    return a;
}

static void resize(ptr_array *a, size_t new_capicity) {
    size_t old_cap = a->capacity;
    void **tmp = realloc(a->data, new_capicity * sizeof(void *));
    a->data = tmp;
    a->capacity = new_capicity;
    // initialize the new portion [old_cap, new_cap)
    if (new_capicity > old_cap) {
        memset(a->data + old_cap, 0, (new_capicity - old_cap) * sizeof(void *));
    }
}

static int ptr_array_push(ptr_array *a, void *value) {
    if (a->length >= a->capacity) {
        // resize
        size_t new_cap = a->capacity ? a->capacity * 2 : 8;
        resize(a, new_cap);
    }

    a->data[a->length++] = value;
    return 0;
}

static int ptr_array_set(ptr_array* a, void* value, int index) {

    if (index >= a->capacity) {
        // resize
        size_t new_cap = (index+1) * 1.4;
        resize(a, new_cap);
    }

    a->data[index] = value;
    a->length = a->length > index+1? a->length : index+1;
    return 0;
}

static void free_ptr_array(ptr_array *a, int free_values) {
    if (!a) return;
    if (free_values && a->data != NULL) {
        for (size_t i = 0; i < a->length; ++i) {
            if (a->data[i] != NULL) {
                free(a->data[i]);
            }
        }
    }
    free(a->data);
    free(a);
}

typedef struct {
    int x;
    int y;
    char code[64];
} AnsiiCode;
ptr_array* last_codes;
ptr_array* codes;
char* last_out;
char* output;
int out_len = 0;
int last_out_len = 0;
int last_out_width = 0;
static void set_chars(int x, int y, char* str) {
    if (x >= 0 && x < term_width && y >= 0 && y < term_height) {
        int index = x + y * term_width;
        size_t n = strlen(str);
        int length = index + n > out_len? out_len - index : n; 
        memcpy(output + index, str, length);
    }
    else {
        printf("Error - trying to set char out of bounds");
        exit(-1);
    }

    // move_cursor(x, y);
    // printf(str);
    // fflush(stdout);
}

static void set_bg_rgb(int x, int y, int r, int g, int b) { 
    int index = x + y * term_width;
    AnsiiCode* code = malloc(sizeof(AnsiiCode));
    code->x = x;
    code->y = y;
    snprintf(code->code, 64, "\033[48;2;%d;%d;%dm", r, g, b);
    ptr_array_set(codes, code, index);

    // move_cursor(x, y);
    // printf(code->code);
}

static void reset_colors(int x, int y) { 
    int index = x + y * term_width;
    AnsiiCode* code = malloc(sizeof(AnsiiCode));
    code->x = x;
    code->y = y;
    snprintf(code->code, 64, "\033[0m");
    ptr_array_set(codes, code, index);

    // move_cursor(x, y);
    // printf(code->code);
}

static void print_output() {
    AnsiiCode empty;
    empty.code[0] = '\0';

    // write_f("last_out.log", last_out);
    // write_f("output.log", output);

    // PRINT out any differences
    go_to_home();
    int last_out_line = 0;
    int out_line = 0;
    AnsiiCode* last_code = &empty;
    AnsiiCode* code = &empty;
    int x = 0;
    int y = 0;
    for (int i = 0; i < out_len; ++i) {

        // account for different lines
        if (i % term_width == 0) {
            out_line +=1;
            // putchar('\n');
            ++y;
            x = 0;
            move_cursor(x, y);
            // c_logf(INFO, "move_cursor(%d, %d)", x, y);
        }
        if (i % last_out_width == 0) {
            last_out_line += 1;
        }

        // check for escape codes
        if (last_codes->length > i && last_codes->data[i] != NULL) {
            last_code = (AnsiiCode*) last_codes->data[i];
        }
        if (codes->length > i && codes->data[i] != NULL) {
            code = (AnsiiCode*) codes->data[i];
            printf(code->code);
            // c_logf(INFO, "printf(%s)", code->code);
            fflush(stdout);
        }
        
        // print out anything different
        int same_char = i < last_out_len && last_out[i] == output[i];
        if (!same_char || last_out_line != out_line || strcmp(last_code->code, code->code) != 0) {
            char c = output[i];
            printf("%c", output[i]);
            // c_logf(INFO, "printf(%c)", output[i]);
            ++x;
        }
        else {
            ++x;
            move_cursor(x, y);
            // c_logf(INFO, "move_cursor(%d, %d)", x, y);
        }

        fflush(stdout);
    }
    fflush(stdout);

    // then wipe ansii codes
    free_ptr_array(last_codes, 1);
    last_codes = codes;

    // set last_out
    free(last_out);
    last_out = output;
    output = NULL;
    last_out_len = out_len;
    last_out_width = term_width;
}

static void init_output() {

    // output strings
    int new_len = term_width * term_height;
    output = malloc(new_len);
    out_len = new_len;
    memset(output, ' ', out_len);

    // ansii arrays
    codes = ptr_array_new();
}


// Color conversions
static void hsv_to_rgb_int(float h, float s, float v, float *r, float *g, float *b)
{
    if (s <= 0.0f)
    {
        int rv = (int)(v * 255 + 0.5f);
        *r = rv;
        *g = rv;
        *b = rv;
        return;
    }
    if (h < 0)
        h = fmodf(h, 360.0f) + 360.0f;
    if (h >= 360.0f)
        h = fmodf(h, 360.0f);
    h /= 60.0f;
    int i = (int)floorf(h);
    float f = h - i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * f);
    float t = v * (1.0f - s * (1.0f - f));
    float r_f = 0, g_f = 0, b_f = 0;
    switch (i % 6)
    {
    case 0:
        r_f = v;
        g_f = t;
        b_f = p;
        break;
    case 1:
        r_f = q;
        g_f = v;
        b_f = p;
        break;
    case 2:
        r_f = p;
        g_f = v;
        b_f = t;
        break;
    case 3:
        r_f = p;
        g_f = q;
        b_f = v;
        break;
    case 4:
        r_f = t;
        g_f = p;
        b_f = v;
        break;
    case 5:
        r_f = v;
        g_f = p;
        b_f = q;
        break;
    default:
        r_f = g_f = b_f = 0;
        break;
    }
    *r = (float)(r_f * 255.0f + 0.5f);
    *g = (float)(g_f * 255.0f + 0.5f);
    *b = (float)(b_f * 255.0f + 0.5f);
}

static void rgb_to_hsv(float r, float g, float b, float *h, float *s, float *v)
{
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    float maxc = fmaxf(rf, fmaxf(gf, bf));
    float minc = fminf(rf, fminf(gf, bf));
    float d = maxc - minc;
    *v = maxc;
    if (maxc == 0.0f)
    {
        *s = 0.0f;
        *h = 0.0f;
        return;
    }
    *s = d / maxc;
    if (d == 0.0f)
    {
        *h = 0.0f;
        return;
    }
    if (maxc == rf)
        *h = (gf - bf) / d + (gf < bf ? 6.0f : 0.0f);
    else if (maxc == gf)
        *h = (bf - rf) / d + 2.0f;
    else
        *h = (rf - gf) / d + 4.0f;
    *h *= 60.0f;
    if (*h < 0)
        *h += 360.0f;
}

static void rgb_to_hsl(float r, float g, float b, float *h, float *s, float *l)
{
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    float maxc = fmaxf(rf, fmaxf(gf, bf));
    float minc = fminf(rf, fminf(gf, bf));
    *l = (maxc + minc) / 2.0f;
    float d = maxc - minc;
    if (d == 0.0f)
    {
        *s = 0.0f;
        *h = 0.0f;
        return;
    }
    *s = (*l > 0.5f ? d / (2.0f - maxc - minc) : d / (maxc + minc));
    if (maxc == rf)
        *h = (gf - bf) / d + (gf < bf ? 6.0f : 0.0f);
    else if (maxc == gf)
        *h = (bf - rf) / d + 2.0f;
    else
        *h = (rf - gf) / d + 4.0f;
    *h *= 60.0f;
    if (*h < 0)
        *h += 360.0f;
}

static void rgb_to_hex(int r, int g, int b, char *buf)
{
    sprintf(buf, "#%02X%02X%02X", r, g, b);
}

static void rgb_to_cmyk(float r, float g, float b, float *c, float *m, float *y, float *k)
{
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    *k = 1.0f - fmaxf(rf, fmaxf(gf, bf));
    if (*k >= 1.0f - 1e-6)
    {
        *c = *m = *y = 0.0f;
        return;
    }
    *c = (1.0f - rf - *k) / (1.0f - *k);
    *m = (1.0f - gf - *k) / (1.0f - *k);
    *y = (1.0f - bf - *k) / (1.0f - *k);
}

static void cmyk_to_rgb(float c, float m, float y, float k, float *r, float *g, float *b)
{
    float rf = (1.0f - c) * (1.0f - k);
    float gf = (1.0f - m) * (1.0f - k);
    float bf = (1.0f - y) * (1.0f - k);
    *r = (float)(rf * 255.0f + 0.5f);
    *g = (float)(gf * 255.0f + 0.5f);
    *b = (float)(bf * 255.0f + 0.5f);
}

static void hsl_to_rgb(float h, float s, float l, float *r, float *g, float *b)
{
    float c = (1.0f - fabsf(2.0f * l - 1.0f)) * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = l - c / 2.0f;
    float rf = 0, gf = 0, bf = 0;
    if (0 <= h && h < 60)
    {
        rf = c;
        gf = x;
        bf = 0;
    }
    else if (60 <= h && h < 120)
    {
        rf = x;
        gf = c;
        bf = 0;
    }
    else if (120 <= h && h < 180)
    {
        rf = 0;
        gf = c;
        bf = x;
    }
    else if (180 <= h && h < 240)
    {
        rf = 0;
        gf = x;
        bf = c;
    }
    else if (240 <= h && h < 300)
    {
        rf = x;
        gf = 0;
        bf = c;
    }
    else
    {
        rf = c;
        gf = 0;
        bf = x;
    }
    *r = (float)((rf + m) * 255.0f + 0.5f);
    *g = (float)((gf + m) * 255.0f + 0.5f);
    *b = (float)((bf + m) * 255.0f + 0.5f);
}

static void rgb_to_rgb_str(float r, float g, float b, char *buf)
{
    sprintf(buf, "%d, %d, %d", r, g, b);
}

// Representation helpers
static void rgb_to_x_y_and_hue(float r, float g, float b, int* x, int* y, int* hue) {
    float h, s, v;
    rgb_to_hsv(r, g, b, &h, &s, &v);
    *x = s * grad_width;
    *y = grad_height - (v * grad_height);
    *hue = h;
}

static void x_y_and_hue_to_rgb(int x, int y, int hue, int* r, int* g, int* b) {
    double sat = x / (double) grad_width;
    double val = (grad_height - y) / (double) grad_height;
    hsv_to_rgb_int(hue, sat, val, r, g, b);
}

static void rgb_to_code_str(char* buf_MAX_INPUT, CodeType code_type, float r, float g, float b) {
    if (code_type == HEX)
    { // HEX
        char hexbuf[16];
        rgb_to_hex(r, g, b, hexbuf);
        snprintf(buf_MAX_INPUT, MAX_INPUT, "%s", hexbuf);
    }
    else if (code_type == RGB)
    { // RGB
        snprintf(buf_MAX_INPUT, MAX_INPUT, "%d,%d,%d", (int)r, (int)g, (int)b);
    }
    else if (code_type == CMYK)
    { // CMYK
        float c, m, y, kf;
        rgb_to_cmyk(r, g, b, &c, &m, &y, &kf);
        snprintf(buf_MAX_INPUT, MAX_INPUT, "%.f%%,%.f%%,%.f%%,%.f%%", c*100.0f, m*100.0f, y*100.0f, kf*100.0f);
    }
    else if (code_type == HSV)
    { // HSV
        float h1, s1, v1;
        rgb_to_hsv(r, g, b, &h1, &s1, &v1);
        snprintf(buf_MAX_INPUT, MAX_INPUT, "%.f,%.f%%,%.f%%", h1, s1*100.0f, v1*100.0f);
    }
    else if (code_type == HSL)
    { // HSL
        float hh, ss, ll;
        rgb_to_hsl(r, g, b, &hh, &ss, &ll);
        snprintf(buf_MAX_INPUT, MAX_INPUT, "%.f,%.f%%,%.f%%", hh, ss*100.0f, ll*100.0f);
    }
}

static void recompute_hsv_hsl_from_rgb()
{
    rgb_to_hsv(cur_r, cur_g, cur_b, &cur_h, &cur_s, &cur_v);
    rgb_to_hsl(cur_r, cur_g, cur_b, &cur_hs, &cur_ss, &cur_ls);
}

static void fill_current_color_from_buffer()
{
    // parse input_buf according to editing mode
    int r, g, b;
    float a;
    if (color_code_type == HEX)
    { // HEX
        const char *s = input_buf;
        while (*s && (*s == '#'))
            s++;
        if (strlen(s) >= 6)
        {
            int rr, gg, bb;
            if (sscanf(s, "%2x%2x%2x", &rr, &gg, &bb) == 3)
            {
                cur_r = rr;
                cur_g = gg;
                cur_b = bb;
                recompute_hsv_hsl_from_rgb();
            }
        }
    }
    else if (color_code_type == RGB)
    { // RGB
        int rr, gg, bb;
        if (sscanf(input_buf, "%d,%d,%d", &rr, &gg, &bb) == 3)
        {
            cur_r = clamp_i(rr, 0, 255);
            cur_g = clamp_i(gg, 0, 255);
            cur_b = clamp_i(bb, 0, 255);
            recompute_hsv_hsl_from_rgb();
        }
    }
    else if (color_code_type == CMYK)
    { // CMYK
        float c, m, y, k;
        if (sscanf(input_buf, "%f%%,%f%%,%f%%,%f%%", &c, &m, &y, &k) == 4)
        {
            float rr, gg, bb;
            c = clamp_f(c/100.0f, 0.0f, 1.0f);
            m = clamp_f(m/100.0f, 0.0f, 1.0f);
            y = clamp_f(y/100.0f, 0.0f, 1.0f);
            k = clamp_f(k/100.0f, 0.0f, 1.0f);
            cmyk_to_rgb(c, m, y, k, &rr, &gg, &bb);
            cur_r = rr;
            cur_g = gg;
            cur_b = bb;
            recompute_hsv_hsl_from_rgb();
        }
    }
    else if (color_code_type == HSV)
    { // HSV
        float h, s, v;
        if (sscanf(input_buf, "%f,%f%%,%f%%", &h, &s, &v) == 3)
        {
            h = fmodf(h, 360.0f);
            if (h < 0)
                h += 360.0f;
            s = clamp_f(s/100.0f, 0.0f, 1.0f);
            v = clamp_f(v/100.0f, 0.0f, 1.0f);
            hsv_to_rgb_int(h, s, v, &cur_r, &cur_g, &cur_b);
            recompute_hsv_hsl_from_rgb();
            hue_line_hue = (int)h;
        }
    }
    else if (color_code_type == HSL)
    { // HSL
        float h, s, l;
        if (sscanf(input_buf, "%f,%f%%,%f%%", &h, &s, &l) == 3)
        {
            h = fmodf(h, 360.0f);
            if (h < 0)
                h += 360.0f;
            s = clamp_f(s/100.0f, 0.0f, 1.0f);
            l = clamp_f(l/100.0f, 0.0f, 1.0f);
            float r, g, b;
            hsl_to_rgb(h, s, l, &r, &g, &b);
            cur_r = r;
            cur_g = g;
            cur_b = b;
            recompute_hsv_hsl_from_rgb();
            hue_line_hue = (int)h;
        }
    }

    rgb_to_x_y_and_hue(cur_r, cur_g, cur_b, &cursor_x, &cursor_y, &hue_line_hue);
    hue_line_x = (hue_line_hue / 360.0f) * hue_line_width; 
}

static void init_color_from_rgb(float r, float g, float b)
{
    cur_r = r;
    cur_g = g;
    cur_b = b;
    recompute_hsv_hsl_from_rgb();
}

static void render()
{
    // sprintf(LOG_FILE, "%d%s", log_c, ".log");
    // log_c++;
    // char s[2];
    // sprintf(s, "%d", log_c % 10);
    // s[1] = '\0';

    // Refresh sizes
    get_terminal_size(&term_width, &term_height);
    init_output();

    // reserve some margins
    int left = 1;
    int top = 1;
    int usable_w = term_width - 2; // margins
    if (usable_w < 20)
        usable_w = 20;
    grad_width = usable_w;
    // gradient height: use about 60% of height for gradient area
    int grad_h = term_height - 14; // reserve lines for hue and info
    if (grad_h < 6)
        grad_h = 6;
    grad_height = grad_h;
    sg = grad_width / 2;
    cg = grad_width - sg;
    if (cursor_x < 0)
        cursor_x = 0;
    if (cursor_x >= grad_width)
        cursor_x = grad_width - 1;


    // Compute current color under cursor if cursor is moving
    if (!editing) {
        x_y_and_hue_to_rgb(cursor_x, cursor_y, hue_line_hue, &cur_r, &cur_g, &cur_b);
    }

    // Draw gradient area
    for (int y = 0; y < grad_height; y++)
    {
        // left grayscale then right color gradient
        for (int x = 0; x < grad_width; x++)
        {
            // make color gradient thing
            float r, g, bv;
            x_y_and_hue_to_rgb(x, y, hue_line_hue, &r, &g, &bv);

            // print block with background color
            set_bg_rgb(left + x, top + y, r, g, bv);
            if (x == cursor_x && y == cursor_y) {
                set_chars(left + x, top + y, "*");
            }
            else {
                set_chars(left + x, top + y, " ");
            }
            // c_logf(INFO, "spot at %d %d", left + x, top + y);
        }
        reset_colors(left + grad_width, top + y);
        // set_chars(left + grad_width, top + y, " ");
    }

    

    // Draw hue line below gradient
    int hue_line_y = top + grad_height + 1;
    set_chars(left, hue_line_y, "- ");
    hue_line_width = grad_width-4;
    for (int x = 0; x < hue_line_width; x++)
    {
        double ratio = (double)x / (double)((grad_width - 1 > 0) ? (grad_width - 1) : 1);
        float r, g, bv;
        hsv_to_rgb_int((float)(ratio * 360.0f), 1.0f, 1.0f, &r, &g, &bv);
        set_bg_rgb(left + 2 + x, hue_line_y, r, g, bv);
        set_chars(left + 2 + x, hue_line_y, " ");
    }
    reset_colors(left + 2 + hue_line_width, hue_line_y);
    set_chars(left + 2 + hue_line_width, hue_line_y, " +");


    // marker on hue line
    set_chars(left + hue_line_x + 2, hue_line_y + 1, "^");


    // Draw info area: current color in various representations
    int r = cur_r, g = cur_g, b = cur_b;

    // show current color
    set_bg_rgb(left, hue_line_y+2, r, g, b);
    set_chars(left, hue_line_y+2, "      ");
    reset_colors(left+5, hue_line_y+2);
    set_bg_rgb(left, hue_line_y+3, r, g, b);
    set_chars(left, hue_line_y+3, "      ");
    reset_colors(left+5, hue_line_y+3);

    // show color codes
    char hex[MAX_INPUT];
    char rgb[MAX_INPUT];
    char cmyk[MAX_INPUT];
    char hsv[MAX_INPUT];
    char hsl[MAX_INPUT];
    rgb_to_code_str(hex, HEX, r, g, b);
    rgb_to_code_str(rgb, RGB, r, g, b);
    rgb_to_code_str(cmyk, CMYK, r, g, b);
    rgb_to_code_str(hsv, HSV, r, g, b);
    rgb_to_code_str(hsl, HSL, r, g, b);

    if (color_code_type == HEX) snprintf(hex, sizeof(hex), "%s", input_buf);
    if (color_code_type == RGB) snprintf(rgb, sizeof(rgb), "%s", input_buf);
    if (color_code_type == CMYK) snprintf(cmyk, sizeof(cmyk), "%s", input_buf);
    if (color_code_type == HSV) snprintf(hsv, sizeof(hsv), "%s", input_buf);
    if (color_code_type == HSL) snprintf(hsl, sizeof(hsl), "%s", input_buf);

    int info_y = hue_line_y + 4;
    if (color_code_type == HEX) {
        set_chars(left, info_y, "HEX   ");
        set_bg_rgb(left+6, info_y, 200, 200, 200);
        set_chars(left+6, info_y, hex);
        reset_colors(left+6+strlen(hex), info_y);
    }
    else {
        char line[MAX_INPUT + 6];
        snprintf(line, sizeof(line),"HEX   %s", hex);
        set_chars(left, info_y, line);
    }

    ++info_y;
    if (color_code_type == RGB) {
        set_chars(left, info_y, "RGB   ");
        set_bg_rgb(left+6, info_y, 200, 200, 200);
        set_chars(left+6, info_y, rgb);
        reset_colors(left+6+strlen(rgb), info_y);
    }
    else {
        char line[MAX_INPUT + 6];
        snprintf(line, sizeof(line),"RGB   %s", rgb);
        set_chars(left, info_y, line);
    }


    ++info_y;
    if (color_code_type == CMYK) {
        set_chars(left, info_y, "CMYK  ");
        set_bg_rgb(left+6, info_y, 200, 200, 200);
        set_chars(left+6, info_y, cmyk);
        reset_colors(left+6+strlen(cmyk), info_y);
    }
    else {
        char line[MAX_INPUT + 6];
        snprintf(line, sizeof(line),"CMYK  %s", cmyk);
        set_chars(left, info_y, line);
    }

    ++info_y;
    if (color_code_type == HSV) {
        set_chars(left, info_y, "HSV   ");
        set_bg_rgb(left+6, info_y, 200, 200, 200);
        set_chars(left+6, info_y, hsv);
        reset_colors(left+6+strlen(hsv), info_y);
    }
    else {
        char line[MAX_INPUT + 6];
        snprintf(line, sizeof(line),"HSV   %s", hsv);
        set_chars(left, info_y, line);
    }

    ++info_y;
    if (color_code_type == HSL) {
        set_chars(left, info_y, "HSL   ");
        set_bg_rgb(left+6, info_y, 200, 200, 200);
        set_chars(left+6, info_y, hsl);
        reset_colors(left+6+strlen(hsl), info_y);
    }
    else {
        char line[MAX_INPUT + 6];
        snprintf(line, sizeof(line),"HSL   %s", hsl);
        set_chars(left, info_y, line);
    }

    ++info_y;
    set_chars(left, info_y, "(Hit TAB to edit color codes, ARROW KEYS to change color saturation and value, +/- for hue, 'q' to quit)");

    print_output();
}

static int parse_hex_str_to_rgb(const char *s, float *r, float *g, float *b)
{
    const char *p = s;
    if (*p == '#')
        p++;
    if (strlen(p) != 6)
        return 0;
    unsigned int rr, gg, bb;
    if (sscanf(p, "%02x%02x%02x", &rr, &gg, &bb) != 3)
        return 0;
    *r = rr;
    *g = gg;
    *b = bb;
    return 1;
}

static void start_edit(CodeType code_type)
{
    editing = 1;
    input_len = 0;
    input_buf[0] = '\0'; // fill initial buffer with current representation
    
    // get color as current color code
    char tmp[MAX_INPUT];
    rgb_to_code_str(tmp, code_type, cur_r, cur_g, cur_b);
    strncpy(input_buf, tmp, MAX_INPUT - 1);
    input_buf[MAX_INPUT - 1] = '\0';
}

static void end_editing()
{
    editing = 0;
    memset(input_buf, 0, sizeof(input_buf));
    input_len = 0;
}

static void handle_key(int ch)
{

    // handle tab editing
    if (ch == 'q' || ch == 'Q')
    { // quit
        exit(0);
    }
    else if (ch == '\t' || ch == '\n' || ch == '\r' || ch == 9)
    {
        if (editing && input_len != 0) {
            fill_current_color_from_buffer();
            end_editing();
        }

        color_code_type = (color_code_type + 1) % 6;
        if (color_code_type != 0) start_edit(color_code_type);

        return;
    }
    else if (ch == 27 && editing) 
    { // escape
        end_editing();
        color_code_type = NONE;
        return;
    }

    // handle editing input
    if (editing)
    { // editing mode
        if (ch == 127 || ch == 8)
        { // backspace
            if (input_len > 0)
            {
                input_buf[--input_len] = '\0';
            }
            else {
                input_buf[0] = '\0';
            }
        }
        else if (ch >= 32 && ch < 127)
        { // printable
            if (input_len < MAX_INPUT - 1)
            {
                input_buf[input_len++] = (char)ch;
                input_buf[input_len] = '\0';
            }
        }
        return;
    }

    // not editing
    if (ch == 0x1b)
    { // escape sequences for arrows
        // read next two chars
        char seq[3] = {0, 0, 0};
        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return;
        if (seq[0] != '[')
        {
            return;
        }
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return;
        if (seq[1] == 'A')
        { // up
            cursor_y = clamp_i(cursor_y - 1, 0, grad_height - 1);
        }
        else if (seq[1] == 'B')
        { // down
            cursor_y = clamp_i(cursor_y + 1, 0, grad_height - 1);
        }
        else if (seq[1] == 'C')
        { // right
            cursor_x = clamp_i(cursor_x + 1, 0, grad_width - 1);
        }
        else if (seq[1] == 'D')
        { // left
            cursor_x = clamp_i(cursor_x - 1, 0, grad_width - 1);
        }
    }
    else
    {
        // direct keys
        if (ch == '-' || ch == '_')
        {
            if (!editing)
            {
                hue_line_x = (hue_line_x - 1) % hue_line_width;
                if (hue_line_x < 0)
                    hue_line_x += hue_line_width;
            }
        }
        else if (ch == '+' || ch == '=')
        {
            if (!editing)
            {
                hue_line_x = (hue_line_x + 1) % hue_line_width;
            }
        }

        hue_line_hue = 360.0f * hue_line_x / (float) hue_line_width;
    }
    return;

}

int main()
{
    // Init terminal
    if (!isatty(STDIN_FILENO))
        return 1;
    set_raw_mode();
    clear_screen();
    go_to_home();


    // initialize color from black
    init_color_from_rgb(100, 150, 220);

    // measure terminal
    get_terminal_size(&term_width, &term_height);
    grad_width = (term_width > 60 ? term_width - 2 : term_width - 2);
    sg = grad_width / 2;
    cg = grad_width - sg;
    cursor_x = sg / 2;
    hue_line_hue = 0;

    // init output
    last_out = malloc(sizeof(char));
    last_out[0] = '\0';
    last_codes = ptr_array_new();
    last_out_width = term_width;
    // init_output();
    // for(int x = 0; x < term_width; ++x) {
    //     for(int y = 0; y < term_height; ++y) {
    //         set_chars(x, y, " ");
    //     }
    // }
    // print_output();

    while (1)
    {
        render();
        char ch;
        ssize_t n = read(STDIN_FILENO, &ch, 1);
        if (n == 1)
        {
            handle_key((int)ch);
        }
        else if (n == 0)
        {
            // EOF or input closed; exit gracefully if you want
            break;
        }
        // if n < 0, you could handle errors here
    }
    return 0;
}
