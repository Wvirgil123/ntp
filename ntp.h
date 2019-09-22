/**
 * \file
 * \brief NT GET UTC TIME
 *
 * \internal
 * \par modification history:
 * - 1.00 19-09-19  vir, first implementation
 * \endinternal
 */

#ifndef __NTP_H__
#define __NTP_H__

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus  */

#include <sys/time.h>
#include <unistd.h>
#include <stdint.h>
#include <bits/stdint-intn.h>

/*
 * \brief get sys time adapter
 */
static inline void ntp_sys_timespec( struct timespec *p_timespec)
{
    struct timeval tv;
    struct timezone tz;
    gettimeofday (&tv , &tz);  // if not, pleas modification

    p_timespec->tv_nsec = tv.tv_usec * 1000;
    p_timespec->tv_sec  = tv.tv_sec;
}


/*
 * \brief get  coarse timespec,
 * \param[in]  p_server_addr  : NTP server
 * \param[out] p_timespec      : coarse timespec
 *
 * retval  0: ok , 1: failed
 */
int ntp_time_get(const char *p_server_addr, struct timespec *p_timespec);

/*
 * \brief get local error time with UTC
 * \param[in]  p_server_addr  : NTP server
 * \param[out] p_tm_diff_ms   : error ms.  (UTC= local_time +  *p_tm_diff_ms)
 *
 * retval  0: ok , 1: failed
 */
int ntp_time_diff_get( const char *p_server_addr, int64_t *p_tm_diff_ms);

/*
 * \brief timespec correct
 * \param[in/out]  p_timespec  : input local timespec, out correct timespec
 * \param[in] p_tm_diff_ms     : error ms.
 */
void timespec_correct(struct timespec *p_timespec, int64_t error_ms);

#ifdef __cplusplus
}
#endif /* __cplusplus   */

#endif /*  */

/* end of file */

