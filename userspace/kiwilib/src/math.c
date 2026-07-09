#include "math.h"

#undef fpclassify
#undef isfinite
#undef isinf
#undef isnan
#undef isnormal
#undef signbit

double fabs(double x) {
    return x < 0.0 ? -x : x;
}

float fabsf(float x) {
    return x < 0.0f ? -x : x;
}

long double fabsl(long double x) {
    return x < 0.0L ? -x : x;
}

double floor(double x) {
    long long i = (long long)x;
    if ((double)i > x) {
        i--;
    }
    return (double)i;
}

float floorf(float x) {
    return (float)floor((double)x);
}

long double floorl(long double x) {
    long long i = (long long)x;
    if ((long double)i > x) {
        i--;
    }
    return (long double)i;
}

double ceil(double x) {
    long long i = (long long)x;
    if ((double)i < x) {
        i++;
    }
    return (double)i;
}

float ceilf(float x) {
    return (float)ceil((double)x);
}

long double ceill(long double x) {
    long long i = (long long)x;
    if ((long double)i < x) {
        i++;
    }
    return (long double)i;
}

double trunc(double x) {
    return (double)((long long)x);
}

float truncf(float x) {
    return (float)trunc((double)x);
}

long double truncl(long double x) {
    return (long double)((long long)x);
}

double round(double x) {
    return x >= 0.0 ? floor(x + 0.5) : ceil(x - 0.5);
}

float roundf(float x) {
    return (float)round((double)x);
}

long double roundl(long double x) {
    return x >= 0.0L ? floorl(x + 0.5L) : ceill(x - 0.5L);
}

double fmod(double x, double y) {
    if (y == 0.0) {
        return NAN;
    }
    return x - trunc(x / y) * y;
}

float fmodf(float x, float y) {
    return (float)fmod((double)x, (double)y);
}

long double fmodl(long double x, long double y) {
    if (y == 0.0L) {
        return NAN;
    }
    return x - truncl(x / y) * y;
}

double copysign(double x, double y) {
    double ax = fabs(x);
    return __builtin_signbit(y) ? -ax : ax;
}

float copysignf(float x, float y) {
    float ax = fabsf(x);
    return __builtin_signbit(y) ? -ax : ax;
}

long double copysignl(long double x, long double y) {
    long double ax = fabsl(x);
    return __builtin_signbit(y) ? -ax : ax;
}

double ldexp(double x, int exp) {
    double factor = 2.0;

    if (exp < 0) {
        exp = -exp;
        factor = 0.5;
    }
    while (exp-- > 0) {
        x *= factor;
    }
    return x;
}

float ldexpf(float x, int exp) {
    return (float)ldexp((double)x, exp);
}

long double ldexpl(long double x, int exp) {
    long double factor = 2.0L;

    if (exp < 0) {
        exp = -exp;
        factor = 0.5L;
    }
    while (exp-- > 0) {
        x *= factor;
    }
    return x;
}

double frexp(double x, int* exp) {
    int e = 0;
    double ax = fabs(x);

    if (!exp) {
        return 0.0;
    }
    if (x == 0.0 || __builtin_isnan(x) || __builtin_isinf(x)) {
        *exp = 0;
        return x;
    }
    while (ax >= 1.0) {
        x *= 0.5;
        ax *= 0.5;
        e++;
    }
    while (ax < 0.5) {
        x *= 2.0;
        ax *= 2.0;
        e--;
    }
    *exp = e;
    return x;
}

float frexpf(float x, int* exp) {
    return (float)frexp((double)x, exp);
}

long double frexpl(long double x, int* exp) {
    int e = 0;
    long double ax = fabsl(x);

    if (!exp) {
        return 0.0L;
    }
    if (x == 0.0L || __builtin_isnan(x) || __builtin_isinf(x)) {
        *exp = 0;
        return x;
    }
    while (ax >= 1.0L) {
        x *= 0.5L;
        ax *= 0.5L;
        e++;
    }
    while (ax < 0.5L) {
        x *= 2.0L;
        ax *= 2.0L;
        e--;
    }
    *exp = e;
    return x;
}

double modf(double x, double* iptr) {
    double i = trunc(x);
    if (iptr) {
        *iptr = i;
    }
    return x - i;
}

float modff(float x, float* iptr) {
    double i = 0.0;
    double frac = modf((double)x, &i);
    if (iptr) {
        *iptr = (float)i;
    }
    return (float)frac;
}

long double modfl(long double x, long double* iptr) {
    long double i = truncl(x);
    if (iptr) {
        *iptr = i;
    }
    return x - i;
}

double sqrt(double x) {
    double guess = x > 1.0 ? x : 1.0;

    if (x < 0.0) {
        return NAN;
    }
    if (x == 0.0) {
        return 0.0;
    }
    for (int i = 0; i < 24; i++) {
        guess = 0.5 * (guess + x / guess);
    }
    return guess;
}

float sqrtf(float x) {
    return (float)sqrt((double)x);
}

long double sqrtl(long double x) {
    return (long double)sqrt((double)x);
}

double pow(double x, double y) {
    long long n = (long long)y;
    double result = 1.0;
    int neg = 0;

    if ((double)n != y) {
        return NAN;
    }
    if (n < 0) {
        neg = 1;
        n = -n;
    }
    while (n-- > 0) {
        result *= x;
    }
    return neg ? 1.0 / result : result;
}

double exp(double x) {
    double term = 1.0;
    double sum = 1.0;

    for (int i = 1; i < 32; i++) {
        term *= x / (double)i;
        sum += term;
    }
    return sum;
}

double log(double x) {
    double y = 0.0;

    if (x <= 0.0) {
        return NAN;
    }
    while (x > 2.0) {
        x *= 0.5;
        y += 0.6931471805599453;
    }
    while (x < 0.5) {
        x *= 2.0;
        y -= 0.6931471805599453;
    }
    x = (x - 1.0) / (x + 1.0);
    {
        double z = x * x;
        double term = x;
        double sum = 0.0;
        for (int n = 1; n < 64; n += 2) {
            sum += term / (double)n;
            term *= z;
        }
        y += 2.0 * sum;
    }
    return y;
}

double log10(double x) {
    return log(x) / 2.302585092994046;
}
