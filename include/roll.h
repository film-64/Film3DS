#ifndef FILM_ROLL_H
#define FILM_ROLL_H
#include "ui.h"

enum { FILM_ROLL_COLUMNS=10, FILM_ROLL_ROWS=3, FILM_ROLL_GRID_X=10,
       FILM_ROLL_GRID_Y=87, FILM_ROLL_CELL=30, FILM_ROLL_SQUARE=24 };
typedef struct {
    unsigned first, pending_index, pending_id;
    bool deleting;
} FilmRoll;
typedef enum {ROLL_DELETE_WAIT,ROLL_DELETE_CONFIRM,ROLL_DELETE_CANCEL} FilmRollDeleteInput;
FilmRollDeleteInput film_roll_delete_input(uint32_t down,uint32_t confirm,uint32_t cancel,uint32_t actions);
FilmStatus film_roll_original_path(char *,size_t,const char *,unsigned);
typedef struct {
    unsigned id,width,height;
    FilmPixelFormat format;
    uint32_t source_crc,jpeg_crc;
} FilmRollPair;
FilmStatus film_roll_pair_publish(const char *,const FilmFrame *,const char *,unsigned);
FilmStatus film_roll_native_raw_pair(const char *,const FilmFrame *raw,const FilmFrame *native,const char *,unsigned);
FilmStatus film_roll_pair_index(const char *,const char *,unsigned,FilmRollPair *);
FilmStatus film_roll_jpeg_preview(const char *,const char *,unsigned,FilmRollPair *,FilmImage *);
FilmStatus film_roll_pair_verify(const char *,const FilmFrame *,const FilmRollPair *);
FilmStatus film_roll_prepare_catalog(const char *,const char *,const char *,const uint16_t *,unsigned,
    bool (*)(unsigned,unsigned,unsigned));
void film_roll_sync(FilmRoll *roll, const FilmUi *ui);
void film_roll_move(FilmRoll *roll, FilmUi *ui, int dx, int dy);
bool film_roll_touch(FilmRoll *roll, FilmUi *ui, unsigned x, unsigned y);
FilmStatus film_roll_request_delete(FilmRoll *roll, const FilmUi *ui, const uint16_t *ids, unsigned count);
void film_roll_cancel_delete(FilmRoll *roll);
FilmStatus film_roll_confirm_delete(FilmRoll *roll, FilmUi *ui, uint16_t *ids, unsigned *count,
                                    const char *directory);
#endif
