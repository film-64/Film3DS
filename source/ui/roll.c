#include "roll.h"
#include "film_runtime_contract.h"
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void film_roll_sync(FilmRoll *roll, const FilmUi *ui)
{
    unsigned row=ui->selection/FILM_ROLL_COLUMNS;
    unsigned first_row=roll->first/FILM_ROLL_COLUMNS;
    if (!ui->photos) first_row=0;
    else if (row<first_row) first_row=row;
    else if (row>=first_row+FILM_ROLL_ROWS) first_row=row-FILM_ROLL_ROWS+1;
    roll->first=first_row*FILM_ROLL_COLUMNS;
}

void film_roll_move(FilmRoll *roll, FilmUi *ui, int dx, int dy)
{
    if (!ui->photos || roll->deleting) return;
    if ((dy<0 && ui->selection<FILM_ROLL_COLUMNS) ||
        (dy>0 && (ui->selection/FILM_ROLL_COLUMNS+1)*FILM_ROLL_COLUMNS>=ui->photos)) dy=0;
    int next=(int)ui->selection+(dx>0?1:dx<0?-1:0)+(dy>0?FILM_ROLL_COLUMNS:dy<0?-FILM_ROLL_COLUMNS:0);
    /* A missing cell in the final row is not selectable; down lands on its last photo. */
    if (next<0) next=0;
    if ((unsigned)next>=ui->photos) next=(int)ui->photos-1;
    ui->selection=(unsigned)next;
    film_roll_sync(roll,ui);
}

bool film_roll_touch(FilmRoll *roll, FilmUi *ui, unsigned x, unsigned y)
{
    if (roll->deleting || x<FILM_ROLL_GRID_X || y<FILM_ROLL_GRID_Y) return false;
    x-=FILM_ROLL_GRID_X;y-=FILM_ROLL_GRID_Y;
    if (x>=FILM_ROLL_COLUMNS*FILM_ROLL_CELL || y>=FILM_ROLL_ROWS*FILM_ROLL_CELL ||
        x%FILM_ROLL_CELL>=FILM_ROLL_SQUARE || y%FILM_ROLL_CELL>=FILM_ROLL_SQUARE) return false;
    unsigned index=roll->first+(y/FILM_ROLL_CELL)*FILM_ROLL_COLUMNS+x/FILM_ROLL_CELL;
    if (index>=ui->photos) return false;
    ui->selection=index;
    return true;
}

FilmStatus film_roll_request_delete(FilmRoll *roll, const FilmUi *ui, const uint16_t *ids, unsigned count)
{
    if (!film_runtime_source_valid("film-roll-delete/v1")) return FILM_STATE;
    if (!roll || !ui || !ids || !count || count!=ui->photos || ui->selection>=count ||
        !ids[ui->selection] || ids[ui->selection]>9999) return FILM_STATE;
    roll->pending_index=ui->selection;roll->pending_id=ids[ui->selection];roll->deleting=true;
    return FILM_OK;
}

void film_roll_cancel_delete(FilmRoll *roll)
{
    roll->deleting=false;roll->pending_id=roll->pending_index=0;
}

FilmRollDeleteInput film_roll_delete_input(uint32_t down,uint32_t confirm,uint32_t cancel,uint32_t actions)
{
    if(down&cancel)return ROLL_DELETE_CANCEL;
    return (down&actions)==confirm?ROLL_DELETE_CONFIRM:ROLL_DELETE_WAIT;
}

FilmStatus film_roll_original_path(char *path,size_t capacity,const char *directory,unsigned id)
{
    if(!path||!capacity)return FILM_INVALID;
    path[0]=0;if(!directory||!*directory||!id||id>9999)return FILM_INVALID;
    size_t n=strlen(directory);while(n&&directory[n-1]=='/')--n;
    if(n>4096)return FILM_INVALID;
    int written=snprintf(path,capacity,"%.*s/ROLL%04u.DNG",(int)n,directory,id);
    if(written<0||(size_t)written>=capacity)return FILM_INVALID;
    struct stat st;if(stat(path,&st)==0)return FILM_OK;
    if(errno!=ENOENT)return FILM_IO;
    memcpy(path+strlen(path)-3,"F3S",3);
    if(stat(path,&st)==0)return FILM_OK;
    if(errno!=ENOENT)return FILM_IO;
    memcpy(path+strlen(path)-3,"DNG",3);return FILM_OK;
}

FilmStatus film_roll_confirm_delete(FilmRoll *roll, FilmUi *ui, uint16_t *ids, unsigned *count,
                                    const char *directory,const char *previews)
{
    if (!film_runtime_source_valid("film-roll-delete/v1")) return FILM_STATE;
    if (!roll || !ui || !ids || !count || !directory || !previews || !roll->deleting) return FILM_STATE;
    unsigned index=roll->pending_index,id=roll->pending_id;
    film_roll_cancel_delete(roll); /* One confirmation permits only one attempt. */
    if (!*count || *count!=ui->photos || index>=*count || ui->selection!=index ||
        ids[index]!=id || !id || id>9999) return FILM_STATE;
    char path[512];
    if(film_roll_original_path(path,sizeof path,directory,id)!=FILM_OK)return FILM_INVALID;
    struct stat st;
    if (stat(path,&st)!=0 || !S_ISREG(st.st_mode)) return FILM_IO;
    char derived[512];
    if(film_roll_original_path(derived,sizeof derived,previews,id)!=FILM_OK)return FILM_INVALID;
    const char *extensions[]={"JPG","F3I","F3C","F3E","F3Y"};
    for(unsigned i=0;i<sizeof extensions/sizeof *extensions;++i){
        memcpy(derived+strlen(derived)-3,extensions[i],3);
        if(unlink(derived)!=0&&errno!=ENOENT)return FILM_IO;
    }
    char duplicate[512];memcpy(duplicate,path,strlen(path)+1);
    memcpy(duplicate+strlen(duplicate)-3,!strcmp(path+strlen(path)-3,"DNG")?"F3S":"DNG",3);
    if(unlink(duplicate)!=0&&errno!=ENOENT)return FILM_IO;
    if(unlink(path)!=0)return FILM_IO;
    memmove(ids+index,ids+index+1,(*count-index-1)*sizeof *ids);
    --*count;ids[*count]=0;ui->photos=*count;
    ui->selection=!*count?0:index<*count?index:*count-1;
    film_roll_sync(roll,ui);
    return FILM_OK;
}
