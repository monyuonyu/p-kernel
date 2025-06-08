# p-kernel libc チュートリアル

## はじめに

このチュートリアルでは、p-kernel libcライブラリの基本的な使用方法を学習します。実際のサンプルコードを通じて、各機能の使い方を理解できます。

## 1. 基本的なセットアップ

### コンパイル環境の準備

```bash
# p-kernel libcをビルド
cd /path/to/p-kernel/lib/libc
make

# テストプログラムのコンパイル例
gcc -I../../include/lib/libc -o myprogram myprogram.c -L. -lc
```

### 基本的なプログラム構造

```c
// basic_example.c
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main()
{
    printf("p-kernel libc へようこそ！\n");
    return 0;
}
```

## 2. 文字列処理の基礎

### 文字列の基本操作

```c
// string_basics.c
#include "string.h"
#include "stdio.h"

int main()
{
    // 文字列の宣言と初期化
    char greeting[50];
    char name[] = "太郎";
    
    // 文字列のコピーと連結
    strcpy(greeting, "こんにちは、");
    strcat(greeting, name);
    strcat(greeting, "さん！");
    
    printf("結果: %s\n", greeting);
    printf("文字列の長さ: %zu文字\n", strlen(greeting));
    
    return 0;
}
```

### 安全な文字列操作

```c
// safe_strings.c
#include "string.h"
#include "stdio.h"

int main()
{
    char buffer[20];
    char source[] = "これは長い文字列です";
    
    // 安全なコピー（バッファオーバーフロー防止）
    strncpy(buffer, source, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';  // 必ず終端文字を設定
    
    printf("安全にコピーされた文字列: %s\n", buffer);
    
    // 文字列比較
    if (strncmp(buffer, source, 5) == 0) {
        printf("最初の5文字が一致しています\n");
    }
    
    return 0;
}
```

### 文字列検索

```c
// string_search.c
#include "string.h"
#include "stdio.h"

int main()
{
    char text[] = "The quick brown fox jumps over the lazy dog";
    char* pos;
    
    // 部分文字列検索
    pos = strstr(text, "fox");
    if (pos) {
        printf("'fox' found at position: %ld\n", pos - text);
    }
    
    // 文字検索
    pos = strchr(text, 'q');
    if (pos) {
        printf("'q' found at position: %ld\n", pos - text);
    }
    
    // 後方検索
    pos = strrchr(text, 'o');
    if (pos) {
        printf("Last 'o' found at position: %ld\n", pos - text);
    }
    
    return 0;
}
```

## 3. 動的メモリ管理

### 基本的なメモリ割り当て

```c
// memory_basic.c
#include "stdlib.h"
#include "string.h"
#include "stdio.h"

int main()
{
    // 整数配列の動的割り当て
    int size = 10;
    int* numbers = (int*)malloc(size * sizeof(int));
    
    if (numbers == NULL) {
        printf("メモリ割り当てに失敗しました\n");
        return 1;
    }
    
    // 配列の初期化
    for (int i = 0; i < size; i++) {
        numbers[i] = i * i;  // 平方数
    }
    
    // 配列の表示
    printf("平方数: ");
    for (int i = 0; i < size; i++) {
        printf("%d ", numbers[i]);
    }
    printf("\n");
    
    // メモリ解放
    free(numbers);
    
    return 0;
}
```

### ゼロ初期化とメモリ再割り当て

```c
// memory_advanced.c
#include "stdlib.h"
#include "stdio.h"

int main()
{
    // ゼロ初期化された配列
    int* array = (int*)calloc(5, sizeof(int));
    if (!array) return 1;
    
    printf("初期値（ゼロ初期化）: ");
    for (int i = 0; i < 5; i++) {
        printf("%d ", array[i]);
    }
    printf("\n");
    
    // 配列サイズを拡張
    array = (int*)realloc(array, 10 * sizeof(int));
    if (!array) return 1;
    
    // 新しい要素に値を設定
    for (int i = 5; i < 10; i++) {
        array[i] = i + 1;
    }
    
    printf("拡張後: ");
    for (int i = 0; i < 10; i++) {
        printf("%d ", array[i]);
    }
    printf("\n");
    
    free(array);
    return 0;
}
```

## 4. 数学計算

### 基本的な数学関数

```c
// math_basic.c
#include "math.h"
#include "stdio.h"

int main()
{
    double x = 2.0, y = 3.0;
    
    // 基本演算
    printf("%.1f の平方根: %.3f\n", x, sqrt(x));
    printf("%.1f の %.1f 乗: %.3f\n", x, y, pow(x, y));
    printf("%.1f の絶対値: %.3f\n", -x, fabs(-x));
    
    // 切り上げ・切り下げ
    double pi = 3.14159;
    printf("π = %.5f\n", pi);
    printf("floor(π) = %.0f\n", floor(pi));
    printf("ceil(π) = %.0f\n", ceil(pi));
    
    return 0;
}
```

### 三角関数

```c
// trigonometry.c
#include "math.h"
#include "stdio.h"

#define PI 3.14159265358979323846

int main()
{
    double angles[] = {0, 30, 45, 60, 90};
    int count = sizeof(angles) / sizeof(angles[0]);
    
    printf("角度\tsin\t\tcos\t\ttan\n");
    printf("--------------------------------------------\n");
    
    for (int i = 0; i < count; i++) {
        double rad = angles[i] * PI / 180.0;  // 度をラジアンに変換
        printf("%.0f°\t%.6f\t%.6f\t%.6f\n", 
               angles[i], sin(rad), cos(rad), tan(rad));
    }
    
    return 0;
}
```

## 5. データ構造の実装例

### 動的配列の実装

```c
// dynamic_array.c
#include "stdlib.h"
#include "string.h"
#include "stdio.h"

typedef struct {
    int* data;
    size_t size;
    size_t capacity;
} DynamicArray;

DynamicArray* array_create(size_t initial_capacity)
{
    DynamicArray* arr = (DynamicArray*)malloc(sizeof(DynamicArray));
    if (!arr) return NULL;
    
    arr->data = (int*)malloc(initial_capacity * sizeof(int));
    if (!arr->data) {
        free(arr);
        return NULL;
    }
    
    arr->size = 0;
    arr->capacity = initial_capacity;
    return arr;
}

int array_push(DynamicArray* arr, int value)
{
    if (arr->size >= arr->capacity) {
        // 容量を2倍に拡張
        size_t new_capacity = arr->capacity * 2;
        int* new_data = (int*)realloc(arr->data, new_capacity * sizeof(int));
        if (!new_data) return 0;  // 失敗
        
        arr->data = new_data;
        arr->capacity = new_capacity;
    }
    
    arr->data[arr->size++] = value;
    return 1;  // 成功
}

void array_destroy(DynamicArray* arr)
{
    if (arr) {
        free(arr->data);
        free(arr);
    }
}

int main()
{
    DynamicArray* arr = array_create(2);
    if (!arr) return 1;
    
    // 要素を追加（自動的に拡張される）
    for (int i = 0; i < 10; i++) {
        array_push(arr, i * 10);
    }
    
    printf("配列の内容: ");
    for (size_t i = 0; i < arr->size; i++) {
        printf("%d ", arr->data[i]);
    }
    printf("\n");
    
    printf("サイズ: %zu, 容量: %zu\n", arr->size, arr->capacity);
    
    array_destroy(arr);
    return 0;
}
```

## 6. 文字処理とテキスト解析

### 文字分類の活用

```c
// text_analysis.c
#include "ctype.h"
#include "string.h"
#include "stdio.h"

void analyze_text(const char* text)
{
    int letters = 0, digits = 0, spaces = 0, others = 0;
    
    for (int i = 0; text[i]; i++) {
        if (isalpha(text[i])) {
            letters++;
        } else if (isdigit(text[i])) {
            digits++;
        } else if (isspace(text[i])) {
            spaces++;
        } else {
            others++;
        }
    }
    
    printf("テキスト解析結果:\n");
    printf("  文字数: %d\n", letters);
    printf("  数字: %d\n", digits);
    printf("  空白: %d\n", spaces);
    printf("  その他: %d\n", others);
    printf("  合計: %d文字\n", letters + digits + spaces + others);
}

void to_title_case(char* str)
{
    int capitalize_next = 1;
    
    for (int i = 0; str[i]; i++) {
        if (isspace(str[i])) {
            capitalize_next = 1;
        } else if (capitalize_next && isalpha(str[i])) {
            str[i] = toupper(str[i]);
            capitalize_next = 0;
        } else if (isalpha(str[i])) {
            str[i] = tolower(str[i]);
        }
    }
}

int main()
{
    char text[] = "hello world 123! this is a TEST.";
    
    printf("元のテキスト: %s\n", text);
    analyze_text(text);
    
    to_title_case(text);
    printf("タイトルケース: %s\n", text);
    
    return 0;
}
```

## 7. ソートとデータ検索

### クイックソートの使用

```c
// sorting_example.c
#include "stdlib.h"
#include "string.h"
#include "stdio.h"

// 整数比較関数
int compare_int(const void* a, const void* b)
{
    return (*(int*)a - *(int*)b);
}

// 文字列比較関数
int compare_string(const void* a, const void* b)
{
    return strcmp(*(const char**)a, *(const char**)b);
}

int main()
{
    // 整数配列のソート
    int numbers[] = {64, 34, 25, 12, 22, 11, 90};
    size_t count = sizeof(numbers) / sizeof(numbers[0]);
    
    printf("ソート前: ");
    for (size_t i = 0; i < count; i++) {
        printf("%d ", numbers[i]);
    }
    printf("\n");
    
    qsort(numbers, count, sizeof(int), compare_int);
    
    printf("ソート後: ");
    for (size_t i = 0; i < count; i++) {
        printf("%d ", numbers[i]);
    }
    printf("\n");
    
    // 二分探索
    int key = 25;
    int* found = (int*)bsearch(&key, numbers, count, sizeof(int), compare_int);
    if (found) {
        printf("%d が見つかりました（位置: %ld）\n", *found, found - numbers);
    }
    
    // 文字列配列のソート
    const char* words[] = {"banana", "apple", "cherry", "date"};
    size_t word_count = sizeof(words) / sizeof(words[0]);
    
    printf("\n文字列ソート前: ");
    for (size_t i = 0; i < word_count; i++) {
        printf("%s ", words[i]);
    }
    printf("\n");
    
    qsort(words, word_count, sizeof(char*), compare_string);
    
    printf("文字列ソート後: ");
    for (size_t i = 0; i < word_count; i++) {
        printf("%s ", words[i]);
    }
    printf("\n");
    
    return 0;
}
```

## 8. エラー処理のベストプラクティス

### 堅牢なエラー処理

```c
// error_handling.c
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "errno.h"

// エラーハンドリング付きのファイル読み書き風関数
int safe_string_copy(char* dest, size_t dest_size, const char* src)
{
    if (!dest || !src || dest_size == 0) {
        return -1;  // 無効な引数
    }
    
    size_t src_len = strlen(src);
    if (src_len >= dest_size) {
        return -2;  // バッファサイズ不足
    }
    
    strcpy(dest, src);
    return 0;  // 成功
}

// メモリ割り当てのエラーハンドリング
int* create_array(size_t size, int default_value)
{
    if (size == 0) {
        printf("エラー: サイズが0です\n");
        return NULL;
    }
    
    int* array = (int*)malloc(size * sizeof(int));
    if (!array) {
        printf("エラー: メモリ割り当てに失敗しました\n");
        return NULL;
    }
    
    // 初期化
    for (size_t i = 0; i < size; i++) {
        array[i] = default_value;
    }
    
    return array;
}

int main()
{
    // 文字列コピーのテスト
    char buffer[10];
    int result = safe_string_copy(buffer, sizeof(buffer), "Hello");
    
    if (result == 0) {
        printf("コピー成功: %s\n", buffer);
    } else {
        printf("コピー失敗: エラーコード %d\n", result);
    }
    
    // 長すぎる文字列のテスト
    result = safe_string_copy(buffer, sizeof(buffer), "This is too long");
    if (result != 0) {
        printf("予想通りエラー: エラーコード %d\n", result);
    }
    
    // メモリ割り当てのテスト
    int* arr = create_array(5, 42);
    if (arr) {
        printf("配列作成成功: ");
        for (int i = 0; i < 5; i++) {
            printf("%d ", arr[i]);
        }
        printf("\n");
        free(arr);
    }
    
    return 0;
}
```

## 9. パフォーマンス最適化のヒント

### 効率的なメモリ使用

```c
// performance_tips.c
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "time.h"

// 非効率な例：文字列を何度も再割り当て
char* inefficient_concat(const char* strings[], size_t count)
{
    char* result = (char*)malloc(1);
    result[0] = '\0';
    
    for (size_t i = 0; i < count; i++) {
        size_t old_len = strlen(result);
        size_t new_len = old_len + strlen(strings[i]);
        result = (char*)realloc(result, new_len + 1);
        strcat(result, strings[i]);
    }
    
    return result;
}

// 効率的な例：必要なサイズを事前に計算
char* efficient_concat(const char* strings[], size_t count)
{
    // 必要な総サイズを計算
    size_t total_len = 0;
    for (size_t i = 0; i < count; i++) {
        total_len += strlen(strings[i]);
    }
    
    // 一度だけメモリを割り当て
    char* result = (char*)malloc(total_len + 1);
    result[0] = '\0';
    
    // 文字列を連結
    for (size_t i = 0; i < count; i++) {
        strcat(result, strings[i]);
    }
    
    return result;
}

int main()
{
    const char* words[] = {"Hello", " ", "World", "!", " ", "Test", " ", "String"};
    size_t count = sizeof(words) / sizeof(words[0]);
    
    // パフォーマンス比較（簡易版）
    clock_t start, end;
    
    // 非効率な方法
    start = clock();
    char* result1 = inefficient_concat(words, count);
    end = clock();
    printf("非効率な方法: %s\n", result1);
    printf("時間: %ld クロック\n", end - start);
    
    // 効率的な方法
    start = clock();
    char* result2 = efficient_concat(words, count);
    end = clock();
    printf("効率的な方法: %s\n", result2);
    printf("時間: %ld クロック\n", end - start);
    
    free(result1);
    free(result2);
    
    return 0;
}
```

## まとめ

このチュートリアルでは、p-kernel libcの主要な機能を実際のコード例とともに学習しました。

### 重要なポイント:

1. **メモリ管理**: 必ずmalloc/freeのペアを守る
2. **文字列操作**: バッファオーバーフローに注意
3. **エラー処理**: 戻り値やNULLポインタのチェック
4. **パフォーマンス**: 事前計算でメモリ再割り当てを最小化

### 次のステップ:

- [API リファレンス](API_REFERENCE.md)で詳細な関数仕様を確認
- 実際のプロジェクトでライブラリを使用
- カーネル開発での活用方法を学習

p-kernel libcを使った開発をお楽しみください！