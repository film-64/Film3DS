#include "roll.h"
#include "film_runtime_contract.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void grid(void)
{
    FilmUi ui=film_ui_new(0);FilmRoll roll={0};ui.page=FILM_ROLL;
    film_roll_move(&roll,&ui,1,1);assert(!ui.selection && !roll.first);
    assert(!film_roll_touch(&roll,&ui,10,87));
    assert(film_ui_key(&ui,UI_ACCEPT)==UI_NONE && ui.page==FILM_ROLL);
    ui.photos=47;ui.selection=5;
    film_roll_move(&roll,&ui,0,-1);assert(ui.selection==5);
    film_roll_move(&roll,&ui,1,0);assert(ui.selection==6);
    film_roll_move(&roll,&ui,0,1);assert(ui.selection==16);
    film_roll_move(&roll,&ui,0,1);assert(ui.selection==26 && !roll.first);
    film_roll_move(&roll,&ui,0,1);assert(ui.selection==36 && roll.first==10);
    film_roll_move(&roll,&ui,0,1);assert(ui.selection==46 && roll.first==20);
    film_roll_move(&roll,&ui,0,1);assert(ui.selection==46);
    film_roll_move(&roll,&ui,0,-1);assert(ui.selection==36);
    ui.selection=39;film_roll_move(&roll,&ui,0,1);assert(ui.selection==46);
    ui.selection=20;film_roll_move(&roll,&ui,0,-1);assert(ui.selection==10 && roll.first==10);
    film_roll_move(&roll,&ui,0,-1);assert(ui.selection==0 && !roll.first);
    assert(film_roll_touch(&roll,&ui,10+9*30+10,87+2*30+10) && ui.selection==29);
    assert(!film_roll_touch(&roll,&ui,10+24,87)); /* gap */
    assert(!film_roll_touch(&roll,&ui,310,87));
    assert(!film_roll_touch(&roll,&ui,10,177));
    ui.selection=46;film_roll_sync(&roll,&ui);assert(roll.first==20);
    assert(!film_roll_touch(&roll,&ui,10+9*30,87+2*30)); /* no photo 50 */
    assert(ui.selection==46);
    assert(film_ui_key(&ui,UI_ACCEPT)==UI_NONE && ui.page==FILM_DARKROOM);
    for (unsigned count=1;count<=9999;++count) {
        ui.photos=count;ui.selection=count-1;film_roll_sync(&roll,&ui);
        assert(roll.first%10==0 && ui.selection>=roll.first && ui.selection<roll.first+30);
    }
    puts("PASS 10-column grid, 3-row scrolling, empty/partial rows, touch gaps, 9999-file bounds and A opens darkroom");
}

static void create(const char *directory,unsigned id)
{
    char path[512];snprintf(path,sizeof path,"%s/ROLL%04u.F3S",directory,id);
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite("F3S",1,3,f)==3);assert(fclose(f)==0);
}
static int exists(const char *directory,unsigned id)
{
    char path[512];snprintf(path,sizeof path,"%s/ROLL%04u.F3S",directory,id);return access(path,F_OK)==0;
}
static void deletion(void)
{
    char canonical[128];
    assert(film_roll_original_path(canonical,sizeof canonical,"sdmc:/3ds/Film3DS/rolls/",2)==FILM_OK);
    assert(!strcmp(canonical,"sdmc:/3ds/Film3DS/rolls/ROLL0002.DNG"));
    assert(film_roll_original_path(canonical,sizeof canonical,"sdmc:/3ds/Film3DS/rolls///",2)==FILM_OK);
    assert(!strcmp(canonical,"sdmc:/3ds/Film3DS/rolls/ROLL0002.DNG"));
    enum {A=1,B=2,X=4,Y=8,DIRECTION=16,TOUCH=32};
    assert(film_roll_delete_input(A,A,X,A|B|X|Y)==ROLL_DELETE_CONFIRM);
    assert(film_roll_delete_input(A|DIRECTION|TOUCH,A,X,A|B|X|Y)==ROLL_DELETE_CONFIRM);
    assert(film_roll_delete_input(A|Y,A,X,A|B|X|Y)==ROLL_DELETE_WAIT);
    assert(film_roll_delete_input(A|X,A,X,A|B|X|Y)==ROLL_DELETE_CANCEL);
    assert(film_roll_delete_input(DIRECTION|TOUCH,A,X,A|B|X|Y)==ROLL_DELETE_WAIT);
    char directory[]="build/roll-test.XXXXXX";
    int temporary=mkstemp(directory);
    assert(temporary>=0 && close(temporary)==0 && unlink(directory)==0 && mkdir(directory,0700)==0);
    char exported[512];snprintf(exported,sizeof exported,"%s/export.JPG",directory);
    FILE *f=fopen(exported,"wb");assert(f);fputs("preserve",f);fclose(f);
    char preview[512],index[512],cache[512],curve[512];
    snprintf(preview,sizeof preview,"%s/ROLL0002.JPG",directory);
    snprintf(index,sizeof index,"%s/ROLL0002.F3I",directory);
    f=fopen(preview,"wb");assert(f);fputs("preview",f);fclose(f);
    f=fopen(index,"wb");assert(f);fputs("index",f);fclose(f);
    snprintf(cache,sizeof cache,"%s/ROLL0002.F3C",directory);
    f=fopen(cache,"wb");assert(f);fputs("cache",f);fclose(f);
    snprintf(curve,sizeof curve,"%s/ROLL0002.F3E",directory);
    f=fopen(curve,"wb");assert(f);fputs("matrix",f);fclose(f);
    uint16_t ids[4]={1,2,4};unsigned count=3;
    FilmUi ui=film_ui_new(count);ui.page=FILM_ROLL;ui.selection=1;FilmRoll roll={0};
    create(directory,1);create(directory,2);create(directory,4);
    assert(film_roll_confirm_delete(&roll,&ui,ids,&count,directory,directory)==FILM_STATE);
    assert(film_roll_request_delete(&roll,&ui,ids,count)==FILM_OK && roll.pending_id==2);
    film_roll_move(&roll,&ui,1,0);assert(ui.selection==1);
    assert(!film_roll_touch(&roll,&ui,10,87));
    film_roll_cancel_delete(&roll);
    assert(film_roll_confirm_delete(&roll,&ui,ids,&count,directory,directory)==FILM_STATE && exists(directory,2) && count==3);
    assert(film_roll_request_delete(&roll,&ui,ids,count)==FILM_OK);
    ids[1]=3;assert(film_roll_confirm_delete(&roll,&ui,ids,&count,directory,directory)==FILM_STATE && count==3 && exists(directory,2));ids[1]=2;
    assert(film_roll_request_delete(&roll,&ui,ids,count)==FILM_OK);
    ui.selection=2;assert(film_roll_confirm_delete(&roll,&ui,ids,&count,directory,directory)==FILM_STATE && exists(directory,2));ui.selection=1;
    assert(film_roll_request_delete(&roll,&ui,ids,count)==FILM_OK);
    assert(film_roll_confirm_delete(&roll,&ui,ids,&count,"build/no-such-roll-directory",directory)==FILM_IO && count==3 && ids[1]==2 && !roll.deleting);
    assert(film_roll_request_delete(&roll,&ui,ids,count)==FILM_OK);
    assert(unlink(index)==0&&mkdir(index,0700)==0);
    assert(film_roll_confirm_delete(&roll,&ui,ids,&count,directory,directory)==FILM_IO);
    assert(exists(directory,2)&&count==3&&ids[1]==2);
    assert(rmdir(index)==0);
    assert(film_roll_request_delete(&roll,&ui,ids,count)==FILM_OK);
    char trailing[512];snprintf(trailing,sizeof trailing,"%s/",directory);
    assert(film_roll_confirm_delete(&roll,&ui,ids,&count,trailing,directory)==FILM_OK);
    assert(access(preview,F_OK)!=0&&access(index,F_OK)!=0&&access(cache,F_OK)!=0&&access(curve,F_OK)!=0);
    assert(count==2 && ui.photos==2 && ui.selection==1 && ids[0]==1 && ids[1]==4 && !exists(directory,2));
    assert(exists(directory,1)&&exists(directory,4)&&access(exported,F_OK)==0);
    assert(film_roll_confirm_delete(&roll,&ui,ids,&count,directory,directory)==FILM_STATE);
    assert(film_roll_request_delete(&roll,&ui,ids,count)==FILM_OK);
    assert(film_roll_confirm_delete(&roll,&ui,ids,&count,directory,directory)==FILM_OK && count==1 && ui.selection==0 && ids[0]==1);
    assert(film_roll_request_delete(&roll,&ui,ids,count)==FILM_OK);
    assert(film_roll_confirm_delete(&roll,&ui,ids,&count,directory,directory)==FILM_OK && !count && !ui.photos && !ui.selection && !roll.first);
    assert(film_roll_request_delete(&roll,&ui,ids,count)==FILM_STATE);
    assert(film_ui_key(&ui,UI_ACCEPT)==UI_NONE && ui.page==FILM_ROLL);
    f=fopen(exported,"rb");assert(f);char retained[9]={0};assert(fread(retained,1,8,f)==8 && !strcmp(retained,"preserve"));fclose(f);
    assert(unlink(exported)==0 && rmdir(directory)==0);
    puts("PASS delete confirmation snapshot, cancel/no writes, stale ID/selection rejection, error preserves list, next/previous/empty selection, derived JPEG/index removed, export retained and no retries");
}
static void decode_selection(void)
{
    FilmRollDecode *batch=calloc(1,sizeof *batch);assert(batch);
    uint16_t ids[]={1,3,8};
    assert(film_roll_decode_begin(batch,ids,3,1)==FILM_OK);
    assert(batch->decode_active&&batch->decode_count==1&&batch->decode_ids[0]==3);
    assert(film_roll_decode_toggle(batch,1)==FILM_STATE);
    film_roll_decode_advance(batch,true);assert(!batch->decode_active);
    assert(film_roll_decode_toggle(batch,1)==FILM_OK&&film_roll_decode_toggle(batch,8)==FILM_OK);
    assert(film_roll_decode_begin(batch,ids,3,1)==FILM_OK&&batch->decode_count==2);
    assert(batch->decode_ids[0]==1&&batch->decode_ids[1]==8);
    film_roll_decode_advance(batch,true);
    assert(batch->decode_position==1&&!batch->decode_selection[1]&&batch->decode_selection[8]);
    film_roll_decode_cancel(batch);assert(!batch->decode_active&&batch->decode_selection[8]);
    assert(film_roll_decode_begin(batch,ids,3,0)==FILM_OK&&batch->decode_ids[0]==8);
    film_roll_decode_advance(batch,false);assert(!batch->decode_active&&batch->decode_selection[8]);
    film_roll_decode_cancel(batch);batch->decode_selection[1]=batch->decode_selection[8]=true;
    assert(film_roll_decode_begin(batch,ids,3,0)==FILM_OK&&batch->decode_count==2);
    assert(batch->decode_ids[0]==8&&batch->decode_ids[1]==1); /* Selected scene is retained last. */
    film_roll_decode_cancel(batch);
    uint16_t bad[]={3,3};assert(film_roll_decode_begin(batch,bad,2,0)==FILM_INVALID&&!batch->decode_active);
    free(batch);puts("PASS single decode, marked-ID snapshot, one-at-a-time advance, cancel/error retain unprocessed selection");
}
int main(void)
{
    assert(film_runtime_source_valid("film-roll-preview/v1") && film_runtime_source_valid("film-roll-delete/v1"));
    assert(!film_runtime_source_valid("film-roll-delete/unknown") && !film_runtime_source_valid("scheduler_decision:admitted"));
    grid();deletion();decode_selection();return 0;
}
