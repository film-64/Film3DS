#include "film.h"
#include "cie1931_5nm.h"

#include <math.h>

FilmStatus film_blackbody_white(unsigned kelvin, float xyz[3])
{
    if (!xyz || kelvin<1000 || kelvin>10000) return FILM_INVALID;
    double sum[3]={0};
    for (unsigned i=0;i<sizeof film_cie1931/sizeof *film_cie1931;++i) {
        const double *s=film_cie1931[i];
        double power=pow(560/s[0],5)/expm1(1.438776877e7/(s[0]*kelvin));
        for (unsigned c=0;c<3;++c) sum[c]+=power*s[c+1];
    }
    for (unsigned c=0;c<3;++c) xyz[c]=(float)(sum[c]/sum[1]);
    return FILM_OK;
}

FilmStatus film_white_balance_matrix(FilmWhiteBalance wb, float out[3][3])
{
    if (!out || (wb.kelvin && (wb.kelvin<1000 || wb.kelvin>10000)) ||
        wb.red < -9 || wb.red > 9 || wb.blue < -9 || wb.blue > 9) return FILM_INVALID;
    double transform[3][3]={{1,0,0},{0,1,0},{0,0,1}};
    if (wb.kelvin) {
        const double bradford[3][3]={{.8951,.2664,-.1614},{-.7502,1.7135,.0367},{.0389,-.0685,1.0296}};
        const double inverse[3][3]={{.9869929055,-.1470542564,.1599626517},{.4323052697,.5183602715,.0492912282},{-.0085286646,.0400428217,.9684866958}};
        const double rgb_xyz[3][3]={{.4124564,.3575761,.1804375},{.2126729,.7151522,.0721750},{.0193339,.1191920,.9503041}};
        const double xyz_rgb[3][3]={{3.2404542,-1.5371385,-.4985314},{-.9692660,1.8760108,.0415560},{.0556434,-.2040259,1.0572252}};
        const double d65[]={.95047,1,1.08883};
        float source[3];
        if (film_blackbody_white(wb.kelvin,source)!=FILM_OK) return FILM_INVALID;
        double gain[3],cat[3][3]={{0}},temp[3][3]={{0}};
        for (unsigned i=0;i<3;++i) {
            double a=0,b=0;
            for (unsigned j=0;j<3;++j) { a+=bradford[i][j]*source[j]; b+=bradford[i][j]*d65[j]; }
            gain[i]=b/a;
        }
        for (unsigned i=0;i<3;++i) for (unsigned j=0;j<3;++j) {
            for (unsigned k=0;k<3;++k) cat[i][j]+=inverse[i][k]*gain[k]*bradford[k][j];
            transform[i][j]=0;
        }
        for (unsigned i=0;i<3;++i) for (unsigned j=0;j<3;++j)
            for (unsigned k=0;k<3;++k) temp[i][j]+=cat[i][k]*rgb_xyz[k][j];
        for (unsigned i=0;i<3;++i) for (unsigned j=0;j<3;++j)
            for (unsigned k=0;k<3;++k) transform[i][j]+=xyz_rgb[i][k]*temp[k][j];
    }
    double r=exp2(wb.red/12.0), b=exp2(wb.blue/12.0);
    double norm=.2126*r+.7152+.0722*b, gains[]={r/norm,1/norm,b/norm};
    for (unsigned i=0;i<3;++i) for (unsigned j=0;j<3;++j)
        out[i][j]=(float)(transform[i][j]*gains[i]);
    return FILM_OK;
}

static float linear(float x)
{
    return x<=.04045f?x/12.92f:powf((x+.055f)/1.055f,2.4f);
}
static float code(float x)
{
    return x<=.0031308f?12.92f*x:1.055f*powf(x,1/2.4f)-.055f;
}

FilmRgb film_white_balance_apply(FilmRgb rgb, const float matrix[3][3])
{
    float in[]={linear(rgb.r),linear(rgb.g),linear(rgb.b)},out[3];
    for (unsigned i=0;i<3;++i) out[i]=code(matrix[i][0]*in[0]+matrix[i][1]*in[1]+matrix[i][2]*in[2]);
    return (FilmRgb){out[0],out[1],out[2]};
}

FilmRgb film_white_balance_swatch(int red, int blue)
{
    float matrix[3][3];
    if (film_white_balance_matrix((FilmWhiteBalance){0,red,blue},matrix)!=FILM_OK) return (FilmRgb){0};
    return film_white_balance_apply((FilmRgb){.5f,.5f,.5f},matrix);
}
