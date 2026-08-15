#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <assert.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
#include <stddef.h>   /* offsetof */
#include <complex.h>  /* double complex, I, creal, cimag */
#include <errno.h>    /* errno */
#include <signal.h>   /* signal handler example */
#include <stdatomic.h>/* atomic_int, atomic_fetch_add, atomic_load */

/* ============================================
   COMPLEX PREPROCESSOR MACROS
   ============================================ */

#define CONCAT(a, b) a##b
#define STRINGIFY(x) #x
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define FOREACH(i, n) for(int i = 0; i < (n); i++)
#define VECTOR_OP(type, op, ...) \
    ({ \
        type _result = 0; \
        type _args[] = {__VA_ARGS__}; \
        for(size_t _i = 0; _i < sizeof(_args)/sizeof(type); _i++) { \
            _result op##= _args[_i]; \
        } \
        _result; \
    })

#define COMPLEX_MACRO(x, y) \
    ({ \
        typeof(x) _x = (x); \
        typeof(y) _y = (y); \
        _x > _y ? _x : _y; \
    })

/* Recursive macro (advanced) */
#define COUNT_ARGS(...) COUNT_ARGS_IMPL(__VA_ARGS__, 8,7,6,5,4,3,2,1,0)
#define COUNT_ARGS_IMPL(_1,_2,_3,_4,_5,_6,_7,_8,N,...) N

/* ============================================
   COMPLEX DATA STRUCTURES
   ============================================ */

/* Self-referential structures with unions and bitfields */
typedef struct Node {
    int data;
    struct Node* next;
    struct Node* prev;
    
    union {
        struct {
            unsigned int is_red : 1;
            unsigned int is_balanced : 1;
            unsigned int is_leaf : 1;
            unsigned int padding : 29;
        } flags;
        unsigned int flag_bits;
    };
    
    void (*callback)(struct Node*);
} Node;

/* Variable-length array in struct (C99) */
typedef struct {
    int size;
    int capacity;
    int data[];
} DynamicArray;

/* Complex nested structures */
typedef struct Matrix {
    int rows;
    int cols;
    union {
        float data_float[16];
        int data_int[16];
        double data_double[16];
    };
    
    struct {
        int (*add)(struct Matrix*, struct Matrix*, struct Matrix*);
        int (*mul)(struct Matrix*, struct Matrix*, struct Matrix*);
    } operations;
} Matrix;

/* ============================================
   COMPLEX FUNCTION DECLARATIONS
   ============================================ */

/* Function pointers with complex signatures */
typedef int (*BinaryOp)(int, int);
typedef void (*VoidFunc)(void*);
typedef int (*ComplexFunc)(int, ...);

/* Forward declarations with different calling conventions */
int complex_recursive(int n) __attribute__((noinline));
void __attribute__((noreturn)) panic(const char* msg);
static inline __attribute__((always_inline, unused)) int fast_square(int x) {
    return x * x;
}

/* ============================================
   TEST FUNCTIONS
   ============================================ */

/* 1. Complex arithmetic with integer promotion and overflow */
void test_arithmetic_advanced() {
    printf("\n=== Test 1: Advanced Arithmetic ===\n");
    
    /* Mixed type operations */
    unsigned int ui = 0xFFFFFFFF;
    int si = -1;
    long long ll = 0x7FFFFFFFFFFFFFFFLL;
    
    printf("Unsigned: %u, Signed: %d\n", ui, si);
    printf("Unsigned + Signed: %lld\n", (long long)ui + si);
    printf("Overflow check: %lld\n", ll + 1);
    
    /* Floating point precision */
    float f = 0.1f;
    double d = 0.1;
    for(int i = 0; i < 10; i++) {
        f += 0.1f;
        d += 0.1;
    }
    printf("Float sum (11 x 0.1f accumulated in float): %.20f\n", f);
    printf("Double sum (11 x 0.1 accumulated in double): %.20f\n", d);
    
    /* Bitwise operations */
    int a = 0x55, b = 0xAA;
    printf("Bitwise: %d & %d = %d\n", a, b, a & b);
    printf("Bitwise: %d | %d = %d\n", a, b, a | b);
    printf("Bitwise: %d ^ %d = %d\n", a, b, a ^ b);
    printf("Left shift: %d << 2 = %d\n", a, a << 2);
    printf("Right shift (signed): %d >> 2 = %d\n", -16, -16 >> 2);
}

/* 2. Advanced pointer arithmetic and memory manipulation */
void test_pointers_advanced() {
    printf("\n=== Test 2: Advanced Pointers ===\n");
    
    /* Pointer to pointer */
    int x = 42;
    int *p = &x;
    int **pp = &p;
    printf("Triple indirection: **pp = %d\n", **pp);
    
    /* Function pointers array */
    int add(int a, int b) { return a + b; }
    int sub(int a, int b) { return a - b; }
    int mul(int a, int b) { return a * b; }
    int div(int a, int b) { return b ? a / b : 0; }
    
    int (*ops[4])(int, int) = {add, sub, mul, div};
    const char *op_names[] = {"add", "sub", "mul", "div"};
    
    for(int i = 0; i < 4; i++) {
        printf("%s(10, 5) = %d\n", op_names[i], ops[i](10, 5));
    }
    
    /* Pointer arithmetic on arrays */
    int arr[10] = {0,1,2,3,4,5,6,7,8,9};
    int *ptr = arr;
    ptr += 5;
    printf("Array element using pointer: arr[5] = %d, *(arr+5) = %d\n", arr[5], *(arr+5));
    printf("Pointer difference: %ld\n", ptr - arr);
    
    /* Void pointer with casting */
    void *vp = &x;
    int *ip = (int*)vp;
    printf("Void pointer cast: %d\n", *ip);
    
    /* Const and volatile qualifiers */
    const int c = 100;
    volatile int v = 200;
    const volatile int cv = 300;
    printf("Const: %d, Volatile: %d, Const-volatile: %d\n", c, v, cv);
}

/* 3. Advanced control flow - goto, switch, setjmp */
jmp_buf env;

void test_control_flow_advanced() {
    printf("\n=== Test 3: Advanced Control Flow ===\n");
    
    /* Setjmp/longjmp */
    int val = setjmp(env);
    if(val == 0) {
        printf("First time in setjmp\n");
        printf("Jumping back...\n");
        longjmp(env, 1);
    } else {
        printf("Returned from longjmp with value: %d\n", val);
    }
    
    /* Complex switch with fallthrough */
    int value = 3;
    switch(value) {
        case 1:
            printf("Case 1\n");
            // Intentional fallthrough
        case 2:
            printf("Case 2\n");
            // Fallthrough with condition
            if(value < 3) break;
            __attribute__((fallthrough));
        case 3:
            printf("Case 3\n");
            __attribute__((fallthrough));
        default:
            printf("Default case\n");
            break;
    }
    
    /* Goto for error handling */
    int error_code = 0;
    FILE *f = fopen("nonexistent.txt", "r");
    if(!f) {
        error_code = 1;
        goto error_handling;
    }
    
    // Normal flow
    fclose(f);
    goto cleanup;
    
error_handling:
    printf("Error occurred! Code: %d\n", error_code);
    
cleanup:
    printf("Cleanup complete\n");
    
    /* Labeled break/continue */
    for(int i = 0; i < 3; i++) {
        for(int j = 0; j < 3; j++) {
            if(i == 1 && j == 1) {
                printf("Breaking outer loop at [%d,%d]\n", i, j);
                break;
            }
            if(i == 2 && j == 0) {
                printf("Continuing outer loop at [%d,%d]\n", i, j);
                continue;
            }
            printf("[%d,%d] ", i, j);
        }
        printf("\n");
    }
}

/* 4. Complex string and memory operations */
void test_strings_advanced() {
    printf("\n=== Test 4: Advanced Strings ===\n");
    
    /* String literals and concatenation */
    char *str = "Hello " "World" "!";  // Compile-time concatenation
    printf("Concatenated string: %s\n", str);
    
    /* Array of string literals */
    const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    printf("Month: %s\n", months[5]);
    
    /* Complex string manipulation */
    char buffer[100];
    char *p = buffer;
    p += sprintf(p, "Formatted: %d, %s, %.2f", 42, "test", 3.14);
    p += sprintf(p, " | More: %p", (void*)buffer);
    printf("Sprintf result: %s\n", buffer);
    printf("Buffer length: %ld\n", strlen(buffer));
    
    /* String to number conversions */
    char nums[] = "123 45.6 789";
    char *endptr;
    long l = strtol(nums, &endptr, 10);
    double d = strtod(endptr, &endptr);
    int i = (int)strtol(endptr, NULL, 10);
    printf("Parsed numbers: %ld, %.2f, %d\n", l, d, i);
    
    /* Memory manipulation */
    int arr1[10] = {1,2,3,4,5,6,7,8,9,10};
    int arr2[10];
    memcpy(arr2, arr1, sizeof(arr1));
    memset(arr2 + 5, 0, 5 * sizeof(int));
    
    printf("After memcpy and memset: ");
    for(int i = 0; i < 10; i++) {
        printf("%d ", arr2[i]);
    }
    printf("\n");
}

/* 5. Advanced recursion and variadic functions */
int (*callback)(int);

int complex_recursive(int n) {
    if(n <= 0) return 0;
    
    static int call_count = 0;
    call_count++;
    
    // Mutual recursion using function pointer
    if(callback) {
        return callback(n - 1) + n;
    }
    
    // Tail recursion with optimization hint
    return complex_recursive(n - 1) + n;
}

int fibonacci_memoized(int n) {
    static int memo[100] = {0};
    if(n <= 1) return n;
    if(memo[n]) return memo[n];
    memo[n] = fibonacci_memoized(n-1) + fibonacci_memoized(n-2);
    return memo[n];
}

/* Variadic function - sum of integers */
int sum_variadic(int count, ...) {
    va_list args;
    va_start(args, count);
    
    int sum = 0;
    for(int i = 0; i < count; i++) {
        sum += va_arg(args, int);
    }
    
    va_end(args);
    return sum;
}

/* Variadic with format string emulation */
void print_variadic(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

void test_recursion_variadic() {
    printf("\n=== Test 5: Recursion and Variadic ===\n");
    
    // Mutual recursion with callback
    int fibonacci_func(int n) {
        if(n <= 1) return n;
        return fibonacci_func(n - 1) + fibonacci_func(n - 2);
    };
    callback = fibonacci_func;
    
    printf("Complex recursive (with callback): %d\n", complex_recursive(5));
    printf("Fibonacci memoized (10): %d\n", fibonacci_memoized(10));
    printf("Sum variadic (1,2,3,4,5): %d\n", sum_variadic(5, 1, 2, 3, 4, 5));
    
    print_variadic("Variadic print: %s %d %.2f\n", "test", 42, 3.14);
    
    // Lambda-like function with GCC nested functions
    #ifdef __GNUC__
    int square(int x) { return x * x; }
    printf("Nested function (GCC extension): %d\n", square(5));
    #endif
}

/* 6. Advanced structures and unions */
void test_structs_unions_advanced() {
    printf("\n=== Test 6: Advanced Structures ===\n");
    
    /* Bit fields */
    struct BitFields {
        unsigned int a : 3;
        unsigned int b : 5;
        unsigned int c : 8;
        unsigned int d : 16;
    } bits = {7, 31, 255, 65535};
    printf("Bit fields: a=%u, b=%u, c=%u, d=%u\n", bits.a, bits.b, bits.c, bits.d);
    printf("Size of bitfield struct: %lu\n", sizeof(bits));
    
    /* Union with overlapping */
    union Overlap {
        struct {
            unsigned char low;
            unsigned char high;
        } bytes;
        unsigned short word;
    } u;
    u.word = 0x1234;
    printf("Union: word=0x%04x, low=0x%02x, high=0x%02x\n", 
           u.word, u.bytes.low, u.bytes.high);
    
    /* Nested structures with alignment */
    struct __attribute__((packed)) Packed {
        char c;
        int i;
        short s;
    } packed;
    printf("Packed struct size: %lu (char:%lu, int:%lu, short:%lu)\n", 
           sizeof(packed), offsetof(struct Packed, c), 
           offsetof(struct Packed, i), offsetof(struct Packed, s));
    
    /* Flexible array member */
    DynamicArray *da = malloc(sizeof(DynamicArray) + 10 * sizeof(int));
    if(da) {
        da->size = 10;
        da->capacity = 10;
        for(int i = 0; i < 10; i++) {
            da->data[i] = i * i;
        }
        printf("Flexible array: ");
        for(int i = 0; i < da->size; i++) {
            printf("%d ", da->data[i]);
        }
        printf("\n");
        free(da);
    }
}

/* 7. Advanced casting and type punning */
void test_casting_advanced() {
    printf("\n=== Test 7: Advanced Casting ===\n");
    
    /* Type punning through union */
    union FloatInt {
        float f;
        unsigned int i;
    } fi;
    fi.f = 3.14159f;
    printf("Float: %f, Hex: 0x%08x\n", fi.f, fi.i);
    
    /* Pointer casting */
    int arr[4] = {0x11223344, 0x55667788, 0x99AABBCC, 0xDDEEFF00};
    unsigned char *byte_ptr = (unsigned char*)arr;
    printf("Bytes of first int: ");
    for(int i = 0; i < 4; i++) {
        printf("0x%02x ", byte_ptr[i]);
    }
    printf("\n");
    
    /* Function pointer casting (unsafe) */
    int int_func(int x) { return x + 1; }
    void *generic_ptr = (void*)int_func;
    int (*casted_func)(int) = (int(*)(int))generic_ptr;
    printf("Cast function pointer: %d\n", casted_func(10));
    
    /* Integer overflow casting */
    unsigned int ui_max = 0xFFFFFFFF;
    int si_cast = (int)ui_max;
    printf("Unsigned max to signed: %d\n", si_cast);
    
    /* Pointer to integer */
    void *ptr = &arr;
    unsigned long ptr_val = (unsigned long)ptr;
    printf("Pointer as integer: 0x%lx\n", ptr_val);
}

/* 8. Advanced preprocessor and debug */
void test_preprocessor_advanced() {
    printf("\n=== Test 8: Advanced Preprocessor ===\n");
    
    /* Stringification */
    printf("Stringified: %s\n", STRINGIFY(Hello World));
    
    /* Token concatenation */
    int var1 = 10, var2 = 20;
    #define VAR(x) CONCAT(var, x)
    printf("Concatenated: VAR(1) = %d, VAR(2) = %d\n", VAR(1), VAR(2));
    
    /* Variadic macro */
    printf("Min of 1,2,3: %d\n", MIN(MIN(1,2), 3));
    printf("Max of 1,2,3: %d\n", MAX(MAX(1,2), 3));
    
    /* Complex macro expansion */
    int result = VECTOR_OP(int, +, 1, 2, 3, 4, 5);
    printf("Vector sum: %d\n", result);
    
    /* Macro with statement expression (GCC) */
    #ifdef __GNUC__
    int max_val = COMPLEX_MACRO(10, 20);
    printf("Complex macro (GCC): %d\n", max_val);
    #endif
    
    /* Counting arguments */
    #define PRINT_COUNT(...) printf("Argument count: %d\n", COUNT_ARGS(__VA_ARGS__))
    PRINT_COUNT(1, 2, 3, 4, 5);
    PRINT_COUNT(a, b, c);
    
    /* Conditional compilation */
    #ifdef __STDC__
    printf("ANSI C standard: %ld\n", __STDC_VERSION__);
    #endif
    
    #if defined(__linux__) || defined(__unix__)
    printf("Unix/Linux system\n");
    #elif defined(_WIN32)
    printf("Windows system\n");
    #endif
    
    /* Debug macros */
    #define DEBUG_PRINT(fmt, ...) \
        fprintf(stderr, "DEBUG [%s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
    
    DEBUG_PRINT("Variable = %d", 42);
    DEBUG_PRINT("No extra args");
}

/* 9. Complex I/O and file operations */
void test_io_advanced() {
    printf("\n=== Test 9: Advanced I/O ===\n");
    
    /* Formatted I/O */
    char buffer[256];
    int i = 42;
    float f = 3.14;
    char *s = "test";
    
    snprintf(buffer, sizeof(buffer), "i=%d, f=%f, s=%s", i, f, s);
    printf("%s\n", buffer);
    
    /* Scanning with sscanf */
    char *input = "123 45.6 test";
    int scan_i;
    float scan_f;
    char scan_s[10];
    int scanned = sscanf(input, "%d %f %s", &scan_i, &scan_f, scan_s);
    printf("Scanned %d items: %d, %.2f, %s\n", scanned, scan_i, scan_f, scan_s);
    
    /* Temporary file */
    FILE *temp = tmpfile();
    if(temp) {
        fprintf(temp, "Hello temporary file!");
        fseek(temp, 0, SEEK_SET);
        char read_buf[100] = {0};
        fread(read_buf, 1, sizeof(read_buf), temp);
        printf("Temporary file content: %s\n", read_buf);
        fclose(temp);
    }
    
    /* Buffered vs unbuffered */
    static char io_buffer[256];  /* must outlive this function: setbuf keeps the pointer */
    setbuf(stdout, NULL);
    printf("Unbuffered output\n");
    setbuf(stdout, io_buffer);
    printf("Buffered output\n");
    fflush(stdout);
}

/* 10. Complex math and random number generation */
void test_math_advanced() {
    printf("\n=== Test 10: Advanced Math ===\n");
    
    /* Random numbers */
    srand(time(NULL));
    printf("Random numbers: %d, %d, %d\n", rand(), rand(), rand());
    
    /* Mathematical functions */
    double x = 2.0;
    printf("sqrt(2) = %.10f\n", sqrt(x));
    printf("pow(2, 3) = %.0f\n", pow(2, 3));
    printf("sin(pi/2) = %.10f\n", sin(M_PI/2));
    printf("cos(0) = %.10f\n", cos(0));
    printf("log(e) = %.10f\n", log(M_E));
    printf("ceil(2.1) = %.0f\n", ceil(2.1));
    printf("floor(2.9) = %.0f\n", floor(2.9));
    
    /* Complex numbers (C99) */
    #ifdef __STDC_VERSION__
    double complex z = 1.0 + 2.0*I;
    double complex w = 2.0 + 3.0*I;
    double complex result = z * w;
    printf("Complex: (%.1f+%.1fi) * (%.1f+%.1fi) = %.1f+%.1fi\n",
           creal(z), cimag(z), creal(w), cimag(w),
           creal(result), cimag(result));
    #endif
}

/* 11. Error handling and assertions */
void test_error_handling() {
    printf("\n=== Test 11: Error Handling ===\n");
    
    /* Assertions */
    int x = 5;
    assert(x == 5);
    
    /* Errno handling */
    errno = 0;
    FILE *f = fopen("/nonexistent/file.txt", "r");
    if(!f) {
        perror("File open error");
        printf("Error code: %d\n", errno);
    }
    
    /* Signal handling (basic) */
    __attribute__((unused)) void sig_handler(int sig) {
        printf("Caught signal: %d\n", sig);
        // Exit gracefully
        exit(1);
    }
    
    // Uncomment for actual signal testing
    // signal(SIGINT, sig_handler);
    
    /* Setjmp/longjmp error handling */
    void error_cleanup() {
        printf("Error cleanup function\n");
    }
    
    if(setjmp(env) == 0) {
        // Try block
        int *ptr = NULL;
        if(ptr == NULL) {
            longjmp(env, 1);
        }
        *ptr = 42; // This would crash
    } else {
        // Error block
        error_cleanup();
    }
}

/* 12. Complex thread/atomic operations (basic) */
void test_atomic() {
    printf("\n=== Test 12: Atomic Operations ===\n");
    
    #ifdef __STDC_NO_ATOMICS__
    printf("Atomics not supported\n");
    #else
    #include <stdatomic.h>
    atomic_int counter = 0;
    atomic_fetch_add(&counter, 1);
    atomic_fetch_add(&counter, 2);
    printf("Atomic counter: %d\n", atomic_load(&counter));
    #endif
}

/* ============================================
   MAIN - RUN ALL TESTS
   ============================================ */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("========================================\n");
    printf("  COMPLEX COMPILER STRESS TEST SUITE\n");
    printf("========================================\n\n");
    
    printf("Compiler: %s\n", __VERSION__);
    printf("Platform: %s\n", __STDC_HOSTED__ ? "Hosted" : "Freestanding");
    printf("C Standard: %ld\n\n", __STDC_VERSION__);
    
    /* Run all test suites */
    test_arithmetic_advanced();
    test_pointers_advanced();
    test_control_flow_advanced();
    test_strings_advanced();
    test_recursion_variadic();
    test_structs_unions_advanced();
    test_casting_advanced();
    test_preprocessor_advanced();
    test_io_advanced();
    test_math_advanced();
    test_error_handling();
    test_atomic();
    
    printf("\n========================================\n");
    printf("  ALL TESTS COMPLETED\n");
    printf("========================================\n");
    
    return 0;
}