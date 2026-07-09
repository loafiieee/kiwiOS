#ifndef KIWILIB_MATH_H
#define KIWILIB_MATH_H

#define HUGE_VAL  (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define HUGE_VALL (__builtin_huge_vall())
#define INFINITY  (__builtin_inff())
#define NAN       (__builtin_nanf(""))

#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, (x))
#define isfinite(x)   __builtin_isfinite(x)
#define isinf(x)      __builtin_isinf(x)
#define isnan(x)      __builtin_isnan(x)
#define isnormal(x)   __builtin_isnormal(x)
#define signbit(x)    __builtin_signbit(x)

double fabs(double x);
float fabsf(float x);
long double fabsl(long double x);

double floor(double x);
float floorf(float x);
long double floorl(long double x);

double ceil(double x);
float ceilf(float x);
long double ceill(long double x);

double trunc(double x);
float truncf(float x);
long double truncl(long double x);

double round(double x);
float roundf(float x);
long double roundl(long double x);

double fmod(double x, double y);
float fmodf(float x, float y);
long double fmodl(long double x, long double y);

double copysign(double x, double y);
float copysignf(float x, float y);
long double copysignl(long double x, long double y);

double frexp(double x, int* exp);
float frexpf(float x, int* exp);
long double frexpl(long double x, int* exp);

double ldexp(double x, int exp);
float ldexpf(float x, int exp);
long double ldexpl(long double x, int exp);

double modf(double x, double* iptr);
float modff(float x, float* iptr);
long double modfl(long double x, long double* iptr);

double sqrt(double x);
float sqrtf(float x);
long double sqrtl(long double x);

double pow(double x, double y);
double exp(double x);
double log(double x);
double log10(double x);

#endif // KIWILIB_MATH_H
