# p-kernel libc API リファレンス

## 概要

p-kernel libc は ISO C標準に準拠した軽量なC標準ライブラリです。組み込みシステム向けに最適化されており、カーネル開発や教育用途に適しています。

## ヘッダーファイル別API

### ctype.h - 文字分類・変換関数

#### 文字分類関数

```c
int isalnum(int c);    // 英数字判定
int isalpha(int c);    // 英字判定
int iscntrl(int c);    // 制御文字判定
int isdigit(int c);    // 数字判定
int isgraph(int c);    // 印刷可能文字判定（空白以外）
int islower(int c);    // 小文字判定
int isprint(int c);    // 印刷可能文字判定（空白含む）
int ispunct(int c);    // 句読点判定
int isspace(int c);    // 空白文字判定
int isupper(int c);    // 大文字判定
int isxdigit(int c);   // 16進数字判定
```

**使用例:**
```c
#include "ctype.h"

char ch = 'A';
if (isalpha(ch)) {
    printf("'%c' は英字です\n", ch);
}
if (isupper(ch)) {
    printf("'%c' は大文字です\n", ch);
}
```

#### 文字変換関数

```c
int toascii(int c);    // ASCII変換
int tolower(int c);    // 小文字変換
int toupper(int c);    // 大文字変換
```

**使用例:**
```c
char str[] = "Hello World";
for (int i = 0; str[i]; i++) {
    str[i] = toupper(str[i]);
}
// 結果: "HELLO WORLD"
```

### string.h - 文字列・メモリ操作関数

#### メモリ操作関数

```c
void* memcpy(void* dest, const void* src, size_t n);
void* memmove(void* dest, const void* src, size_t n);
void* memchr(const void* s, int c, size_t n);
void* memset(void* s, int c, size_t n);
int   memcmp(const void* s1, const void* s2, size_t n);
```

**使用例:**
```c
char buffer[100];
memset(buffer, 0, sizeof(buffer));  // ゼロクリア
memcpy(buffer, "Hello", 5);         // 文字列コピー

char src[] = "Test";
char dest[10];
memmove(dest, src, strlen(src) + 1); // 安全なコピー
```

#### 文字列操作関数

```c
size_t strlen(const char* s);
char*  strcpy(char* dest, const char* src);
char*  strncpy(char* dest, const char* src, size_t n);
char*  strcat(char* dest, const char* src);
char*  strncat(char* dest, const char* src, size_t n);
int    strcmp(const char* s1, const char* s2);
int    strncmp(const char* s1, const char* s2, size_t n);
```

**使用例:**
```c
char greeting[50] = "Hello";
strcat(greeting, " ");
strcat(greeting, "World");
printf("結果: %s (長さ: %zu)\n", greeting, strlen(greeting));
// 出力: 結果: Hello World (長さ: 11)
```

#### 文字列検索関数

```c
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* haystack, const char* needle);
char* strpbrk(const char* s, const char* accept);
```

**使用例:**
```c
char text[] = "The quick brown fox";
char* pos = strstr(text, "quick");
if (pos) {
    printf("'quick' は位置 %ld で見つかりました\n", pos - text);
}
```

### stdlib.h - 標準ユーティリティ関数

#### メモリ管理関数

```c
void* malloc(size_t size);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);
void  free(void* ptr);
```

**使用例:**
```c
// 100個のint要素を持つ配列を動的割り当て
int* array = (int*)malloc(100 * sizeof(int));
if (array) {
    // 配列を使用
    for (int i = 0; i < 100; i++) {
        array[i] = i;
    }
    free(array);  // メモリ解放
}

// ゼロ初期化された配列
int* zero_array = (int*)calloc(50, sizeof(int));
if (zero_array) {
    // 全要素が0で初期化済み
    free(zero_array);
}
```

#### 数値変換関数

```c
int    atoi(const char* str);
long   atol(const char* str);
double atof(const char* str);
double strtod(const char* str, char** endptr);
long   strtol(const char* str, char** endptr, int base);
unsigned long strtoul(const char* str, char** endptr, int base);
```

**使用例:**
```c
// 基本的な変換
int num = atoi("12345");
double val = atof("3.14159");

// 高機能変換（エラーチェック付き）
char* endptr;
long result = strtol("12345abc", &endptr, 10);
if (*endptr != '\0') {
    printf("変換エラー: '%s' は数値ではありません\n", endptr);
}

// 16進数変換
unsigned long hex = strtoul("0xFF", NULL, 16);  // 255
```

#### 検索・ソート関数

```c
void* bsearch(const void* key, const void* base, size_t nmemb, 
              size_t size, int(*compar)(const void*, const void*));
void  qsort(void* base, size_t nmemb, size_t size,
            int(*compar)(const void*, const void*));
```

**使用例:**
```c
int compare_int(const void* a, const void* b) {
    return (*(int*)a - *(int*)b);
}

int numbers[] = {64, 34, 25, 12, 22, 11, 90};
size_t count = sizeof(numbers) / sizeof(numbers[0]);

// ソート
qsort(numbers, count, sizeof(int), compare_int);

// 二分探索（ソート済み配列に対して）
int key = 25;
int* found = (int*)bsearch(&key, numbers, count, sizeof(int), compare_int);
if (found) {
    printf("%d が見つかりました\n", *found);
}
```

### math.h - 数学関数

#### 基本数学関数

```c
double fabs(double x);         // 絶対値
double sqrt(double x);         // 平方根
double pow(double x, double y); // べき乗
double floor(double x);        // 床関数
double ceil(double x);         // 天井関数
double fmod(double x, double y); // 剰余
```

#### 三角関数

```c
double sin(double x);      // 正弦
double cos(double x);      // 余弦
double tan(double x);      // 正接
double asin(double x);     // 逆正弦
double acos(double x);     // 逆余弦
double atan(double x);     // 逆正接
double atan2(double y, double x); // 2引数逆正接
```

#### 指数・対数関数

```c
double exp(double x);      // 指数関数
double log(double x);      // 自然対数
double log10(double x);    // 常用対数
```

**使用例:**
```c
#include "math.h"

// 基本的な計算
double radius = 5.0;
double area = M_PI * pow(radius, 2.0);  // 円の面積
double diagonal = sqrt(pow(3.0, 2.0) + pow(4.0, 2.0)); // ピタゴラスの定理

// 三角関数（角度をラジアンで指定）
double angle_deg = 45.0;
double angle_rad = angle_deg * M_PI / 180.0;
double sin_val = sin(angle_rad);
double cos_val = cos(angle_rad);

printf("sin(45°) = %f\n", sin_val);
printf("cos(45°) = %f\n", cos_val);
```

### stdio.h - 入出力関数

#### フォーマット出力関数

```c
int printf(const char* format, ...);
int sprintf(char* str, const char* format, ...);
int snprintf(char* str, size_t size, const char* format, ...);
```

**対応フォーマット指定子:**
- `%d`, `%i`: 10進整数
- `%u`: 符号なし10進整数
- `%x`: 16進整数（小文字）
- `%X`: 16進整数（大文字）
- `%c`: 文字
- `%s`: 文字列
- `%p`: ポインタ
- `%%`: リテラル'%'

**使用例:**
```c
int age = 25;
char name[] = "田中";
double height = 175.5;

printf("名前: %s, 年齢: %d歳\n", name, age);

char buffer[100];
snprintf(buffer, sizeof(buffer), "0x%X", 255);
// buffer = "0xFF"
```

### time.h - 時間関数

```c
time_t time(time_t* tloc);       // 現在時刻取得
clock_t clock(void);             // プロセッサ時間取得
double difftime(time_t time1, time_t time0); // 時間差計算
time_t mktime(struct tm* timeptr); // 時刻構造体→time_t変換
```

**使用例:**
```c
#include "time.h"

time_t start = time(NULL);
// 何らかの処理
time_t end = time(NULL);

double elapsed = difftime(end, start);
printf("処理時間: %.2f秒\n", elapsed);
```

## エラー処理

### errno.h

```c
extern int errno;                // グローバルエラー変数
char* strerror(int errnum);      // エラーメッセージ取得
```

**使用例:**
```c
#include "errno.h"

void* ptr = malloc(SIZE_MAX);  // 巨大なメモリ要求（失敗する）
if (ptr == NULL) {
    printf("エラー: %s\n", strerror(errno));
}
```

## コンパイル方法

```bash
# ヘッダーファイルのインクルードパス指定
gcc -I/path/to/p-kernel/include/lib/libc your_program.c

# ライブラリのリンク
gcc your_program.c -L/path/to/p-kernel/lib/libc -lc

# 数学関数を使用する場合
gcc your_program.c -L/path/to/p-kernel/lib/libc -lc -lm
```

## 注意事項

1. **スレッドセーフティ**: 現在の実装はシングルスレッド用です
2. **メモリ管理**: 固定サイズ（64KB）のヒープを使用
3. **精度**: 数学関数は教育・開発用途に適した精度
4. **I/O**: stdio関数は現在フォーマット機能のみ実装

## パフォーマンス

- **メモリ使用量**: ライブラリサイズ 106KB
- **実行速度**: 組み込み環境向けに最適化
- **メモリ効率**: 軽量な実装、最小限のオーバーヘッド