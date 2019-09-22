/**
 * \file
 * \brief NTP client demo
 *
 * \internal
 * \par modification history:
 * - 1.00 19-09-22  vir, first implementation
 * \endinternal
 */

#include "ntp.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>


#define NTP_SERVER   "cn.pool.ntp.org"

int main(void)
{
    int   ret        = 0;
    int64_t error_ms = 0;
    struct timespec timeval;

    while(1) {

        ret = ntp_time_get(NTP_SERVER, &timeval);
        if(ret == 0 ) {

            printf("TIME:%s", ctime( (const time_t *) &timeval.tv_sec));

            ret = ntp_time_diff_get(NTP_SERVER, &error_ms);
            if( ret == 0) {
                printf("error time:%dms\r\n",(int)error_ms);
            }

        }

        sleep(4);
    }
}
