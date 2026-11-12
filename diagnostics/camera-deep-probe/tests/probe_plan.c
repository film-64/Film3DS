#include <assert.h>
#include <stdio.h>
#include "../probes/exposure/probe_plan.h"
int main(void)
{
    ProbeRequest r;
    char *resume[]={"probe","--auto","--direct","--sensor=3","--from-phase=34"};
    assert(probe_parse_request(5,resume,&r) && r.sensor==3 && r.first_phase==34 && r.direct && r.auto_start);
    char *all[]={"probe","--auto","--direct"};
    assert(probe_parse_request(3,all,&r) && !r.sensor && r.first_phase==3);
    char *bad1[]={"probe","--from-phase=34"};assert(!probe_parse_request(2,bad1,&r));
    char *bad2[]={"probe","--sensor=4"};assert(!probe_parse_request(2,bad2,&r));
    char *bad3[]={"probe","--sensor=3junk"};assert(!probe_parse_request(2,bad3,&r));
    char *bad4[]={"probe","--sensor=3","--from-phase=59"};assert(!probe_parse_request(3,bad4,&r));
    char *bad5[]={"probe","--bypass"};assert(!probe_parse_request(2,bad5,&r));
    char *cal[]={"probe","--auto","--calibrate","--sensor=1"};
    assert(probe_parse_request(4,cal,&r) && r.calibrate && r.sensor==1);
    char *cal_bad[]={"probe","--calibrate"};assert(!probe_parse_request(2,cal_bad,&r));
    char *cal_resume[]={"probe","--calibrate","--sensor=1","--from-phase=34"};assert(!probe_parse_request(4,cal_resume,&r));
    char *cal_direct[]={"probe","--calibrate","--direct","--sensor=1"};assert(!probe_parse_request(4,cal_direct,&r));
    char *smoke[]={"probe","--calibrate","--smoke","--sensor=1"};assert(probe_parse_request(4,smoke,&r) && r.smoke);
    char *smoke_bad[]={"probe","--smoke"};assert(!probe_parse_request(2,smoke_bad,&r));
    char *offline[]={"probe","--calibrate","--offline","--sensor=1"};assert(probe_parse_request(4,offline,&r) && r.offline);
    char *offline_bad[]={"probe","--offline"};assert(!probe_parse_request(2,offline_bad,&r));
    char *gain[]={"probe","--auto","--calibrate","--offline","--sensor=1","--calibration-part=gain"};
    assert(probe_parse_request(6,gain,&r) && r.calibration_part==CAL_GAIN && cal_scope_stages(r.calibration_part)==14);
    gain[5]="--calibration-part=slow";assert(probe_parse_request(6,gain,&r) && r.calibration_part==CAL_SLOW);
    gain[5]="--calibration-part=limits";assert(probe_parse_request(6,gain,&r) && r.calibration_part==CAL_LIMITS);
    gain[5]="--calibration-part=digital";assert(probe_parse_request(6,gain,&r) && r.calibration_part==CAL_DIGITAL);
    gain[5]="--calibration-part=codes-fine";assert(probe_parse_request(6,gain,&r) && r.calibration_part==CAL_CODES_FINE);
    gain[5]="--calibration-part=codes-boost";assert(probe_parse_request(6,gain,&r) && r.calibration_part==CAL_CODES_BOOST);
    gain[5]="--calibration-part=digital-headroom";assert(probe_parse_request(6,gain,&r) && r.calibration_part==CAL_DIGITAL_HEADROOM);
    gain[5]="--calibration-part=limits-fffe";assert(probe_parse_request(6,gain,&r) && r.calibration_part==CAL_LIMITS_FFFE);
    gain[5]="--calibration-part=unknown";assert(!probe_parse_request(6,gain,&r));
    gain[5]="--calibration-part=gain";gain[3]="--smoke";assert(!probe_parse_request(6,gain,&r));
    char *part_bad[]={"probe","--calibration-part=gain"};assert(!probe_parse_request(2,part_bad,&r));
    char *ram[]={"probe","--auto","--calibrate","--offline","--sensor=1","--ram","--calibration-part=limits"};
    assert(probe_parse_request(7,ram,&r) && r.ram);
    ram[6]="--smoke";assert(probe_parse_request(7,ram,&r) && r.ram && r.smoke);
    probe_menu_choice(&r,true);assert(r.ram && r.smoke);
    probe_menu_choice(&r,false);assert(r.ram && r.smoke);
    ram[6]="--raw";assert(!probe_parse_request(7,ram,&r));
    char *automatic[]={"probe","--auto","--calibrate","--offline","--sensor=1","--ram","--raw","--calibration-part=exposure-auto"};
    assert(probe_parse_request(8,automatic,&r) && r.calibration_part==CAL_EXPOSURE_AUTO);
    automatic[4]="--sensor=2";assert(!probe_parse_request(8,automatic,&r));
    automatic[4]="--sensor=1";automatic[6]="--direct";assert(!probe_parse_request(8,automatic,&r));
    char *field[]={"field","--calibrate","--offline","--raw","--sensor=1","--automation-fps=5","--calibration-part=exposure-field"};
    assert(probe_parse_request(7,field,&r) && r.calibration_part==CAL_EXPOSURE_FIELD && !r.ram);
    field[4]="--sensor=2";assert(!probe_parse_request(7,field,&r));field[4]="--sensor=1";
    field[5]="--ram";assert(!probe_parse_request(7,field,&r));field[5]="--automation-fps=15";
    assert(!probe_parse_request(7,field,&r));field[5]="--auto";assert(!probe_parse_request(7,field,&r));
    assert(!probe_parse_request(6,ram,&r));
    probe_offline_default(&r);
    assert(r.sensor==1 && r.first_phase==3 && r.calibrate && r.offline && !r.auto_start && !r.direct && !r.smoke);
    puts("PASS explicit diagnostic scope and invalid request rejection");
}
