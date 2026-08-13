export const PRESET_PROGRAMS = {
  hello_world: {
    name: "01 — Hello World",
    code: `#include <stdio.h>

int main() {
    printf("Hello, World! Welcome to Nova Studio Compiler.\\n");
    return 0;
}`
  },
  factorial: {
    name: "02 — Recursive Factorial",
    code: `#include <stdio.h>

int factorial(int n) {
    if (n <= 1) {
        return 1;
    }
    return n * factorial(n - 1);
}

int main() {
    int num = 5;
    int result = factorial(num);
    printf("Factorial of %d = %d\\n", num, result);
    return 0;
}`
  },
  fibonacci: {
    name: "03 — Fibonacci Sequence",
    code: `#include <stdio.h>

int fibonacci(int n) {
    if (n <= 1) return n;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

int main() {
    int terms = 8;
    for (int i = 0; i < terms; i++) {
        printf("%d ", fibonacci(i));
    }
    printf("\\n");
    return 0;
}`
  },
  bubble_sort: {
    name: "04 — Bubble Sort Algorithm",
    code: `#include <stdio.h>

void swap(int *a, int *b) {
    int temp = *a;
    *a = *b;
    *b = temp;
}

int main() {
    int arr[5] = {64, 34, 25, 12, 22};
    int n = 5;

    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (arr[j] > arr[j + 1]) {
                swap(&arr[j], &arr[j + 1]);
            }
        }
    }

    printf("Sorted Array: ");
    for (int i = 0; i < n; i++) {
        printf("%d ", arr[i]);
    }
    printf("\\n");
    return 0;
}`
  },
  array_sum: {
    name: "05 — Array Sum (For Loop)",
    code: `#include <stdio.h>

int main() {
    int numbers[5] = {10, 20, 30, 40, 50};
    int sum = 0;

    for (int i = 0; i < 5; i++) {
        sum += numbers[i];
    }

    printf("Total Sum = %d\\n", sum);
    return 0;
}`
  },
  struct_demo: {
    name: "06 — Structs & Memory Fields",
    code: `#include <stdio.h>

struct Student {
    char name[32];
    int id;
    float gpa;
};

int main() {
    struct Student s1;
    s1.id = 101;
    s1.gpa = 3.92;

    printf("Student ID: %d, GPA: %.2f\\n", s1.id, s1.gpa);
    return 0;
}`
  }
};