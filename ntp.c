/**
 * \file
 * \brief NTP GET UTC TIME
 *
 * \internal
 * \par modification history:
 * - 1.00 19-09-19  vir, first implementation
 * \endinternal
 */
#include "ntp.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <stdio.h>

#define NTP_DEBUG

#ifdef  NTP_DEBUG
#define NTP_DBG(...)   { printf("[NTP]:"); (void)printf(__VA_ARGS__);}
#else
#define NTP_DBG(...)
#endif

#define  NTP_GET_TIME_ACK_TIMEOUT_S   1

#define  NTP_PORT_STR   "123"

#define LI    0
#define VN    3  //Version number of the protocol.
#define MODE  3  //client mode

#define STRATUM 0
#define POLL  4
#define PREC -6


/**
 * \brief  ntc server from  1990 , utc from 1970
 */
#define JAN_1970            0x83aa7e80   /* 2208988800 1970 - 1900 in seconds */

/* How to multiply by 4294.967296 quickly (and not quite exactly)
 * without using floating point or greater than 32-bit integers.
 * If you want to fix the last 12 microseconds of error, add in
 * (2911*(x))>>28)
 */
#define NTPFRAC(x) (4294 * (x) + ((1981 * (x))>>11))

/* The reverse of the above, needed if we want to set our microsecond
 * clock (via clock_settime) based on the incoming time in NTP format.
 * Basically exact.
 */
#define USEC(x) (((x) >> 12) - 759 * ((((x) >> 10) + 32768) >> 16))

/* Converts NTP delay and dispersion, apparently in seconds scaled
 * by 65536, to microseconds.  RFC-1305 states this time is in seconds,
 * doesn't mention the scaling.
 * Should somehow be the same as 1000000 * x / 65536
 */
#define sec2u(x) ( (x) * 15.2587890625 )

/**
 * \brief NTP  packet
 *
 *
 *                          NTPv3
 *
 *    0   2     5     8               16              24              32
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |LI | VN  |Mode |    Stratum    |     Poll      |   Precision   |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |                          Root Delay                           |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |                       Root Dispersion                         |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |                     Reference Identifier                      |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |                                                               |
 *    |                   Reference Timestamp (64)                    |
 *    |                                                               |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |                                                               |
 *    |                   Originate Timestamp (64)                    |
 *    |                                                               |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |                                                               |
 *    |                   Receive Timestamp (64)                      |
 *    |                                                               |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |                                                               |
 *    |                   Transmit Timestamp (64)                     |
 *    |                                                               |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |                                                               |
 *    |                  Authentication (optional) (64)               |
 *    |                                                               |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 *    Originate Timestamp : client send ntp packet time T1
 *    Receive Timestamp   : server recv ntp packet time T2
 *    Transmit Timestamp  : server send ntp packet time T3
 *    client  recv server ntp packet time T4
 *
 * \   error_time = ((T2 -T1) + ( T3 -T4)) / 2
 *
 */

typedef struct aw_ntp_packet{

/*
    uint8_t li_vn_mode;      // Eight bits. li, vn, and mode.
                             // li.   Two bits.   Leap indicator.
                             // vn.   Three bits. Version number of the protocol.
                             // mode. Three bits. Client will pick mode 3 for client.

    uint8_t stratum;         // Eight bits. Stratum level of the local clock.
    uint8_t poll;            // Eight bits. Maximum interval between successive messages.
    uint8_t precision;       // Eight bits. Precision of the local clock.
*/
  volatile uint32_t  head;

  volatile uint32_t rootDelay;      // 32 bits. Total round trip delay time.
  volatile uint32_t rootDispersion; // 32 bits. Max error aloud from primary clock source.
  volatile uint32_t refId;          // 32 bits. Reference clock identifier.

  volatile uint32_t refTm_s;        // 32 bits. Reference time-stamp seconds.
  volatile uint32_t refTm_f;        // 32 bits. Reference time-stamp fraction of a second.

  volatile uint32_t origTm_s;       // 32 bits. Originate time-stamp seconds.
  volatile uint32_t origTm_f;       // 32 bits. Originate time-stamp fraction of a second.

  volatile uint32_t rxTm_s;         // 32 bits. Received time-stamp seconds.
  volatile uint32_t rxTm_f;         // 32 bits. Received time-stamp fraction of a second.

  volatile uint32_t txTm_s;         // 32 bits and the most important field the client cares about. Transmit time-stamp seconds.
  volatile uint32_t txTm_f;         // 32 bits. Transmit time-stamp fraction of a second.

} aw_ntp_packet_t;              // Total: 384 bits or 48 bytes.

struct ntptime {
    uint32_t coarse;
    uint32_t fine;
};

typedef void (*pfn_ntp_time_calc)(aw_ntp_packet_t *p_packet, void *p_arg);

static void __timespec2ntptime(struct timespec *p_timespec, struct ntptime *p_ntp_time)
{
    p_ntp_time->coarse = p_timespec->tv_sec + JAN_1970;
    p_ntp_time->fine   = NTPFRAC(p_timespec->tv_nsec / 1000);
}

static void __ntptime2timespec( struct ntptime *p_ntp_time, struct timespec *p_timespec)
{
    p_timespec->tv_sec    = p_ntp_time->coarse - JAN_1970;
    p_timespec->tv_nsec   = USEC( p_ntp_time->fine) * 1000;
}

static void __send_ntp_packet_htonl( aw_ntp_packet_t *p_packet)
{
    p_packet->head           = htonl(p_packet->head);
    p_packet->rootDelay      = htonl(p_packet->rootDelay);
    p_packet->rootDispersion = htonl(p_packet->rootDispersion);
    p_packet->refId          = htonl(p_packet->refId);

    p_packet->refTm_s        = htonl(p_packet->refTm_s);
    p_packet->refTm_f        = htonl(p_packet->refTm_f);

    p_packet->origTm_s       = htonl(p_packet->origTm_s);
    p_packet->origTm_f       = htonl(p_packet->origTm_f);

    p_packet->rxTm_s         = htonl(p_packet->rxTm_s);
    p_packet->rxTm_f         = htonl(p_packet->rxTm_f);

    p_packet->txTm_s         = htonl(p_packet->txTm_s);
    p_packet->txTm_f         = htonl(p_packet->txTm_f);

}

static void __recv_ntp_packet_ntohl ( aw_ntp_packet_t *p_packet)
{
    p_packet->head           = ntohl(p_packet->head);
    p_packet->rootDelay      = ntohl(p_packet->rootDelay);
    p_packet->rootDispersion = ntohl(p_packet->rootDispersion);
    p_packet->refId          = ntohl(p_packet->refId);

    p_packet->refTm_s        = ntohl(p_packet->refTm_s);
    p_packet->refTm_f        = ntohl(p_packet->refTm_f);

    p_packet->origTm_s       = ntohl(p_packet->origTm_s);
    p_packet->origTm_f       = ntohl(p_packet->origTm_f);

    p_packet->rxTm_s         = ntohl(p_packet->rxTm_s);
    p_packet->rxTm_f         = ntohl(p_packet->rxTm_f);

    p_packet->txTm_s         = ntohl(p_packet->txTm_s);
    p_packet->txTm_f         = ntohl(p_packet->txTm_f);
}

static int __ntp_packet_make( aw_ntp_packet_t *p_packet)
{
    struct timespec timespec;
    struct ntptime     ntp_time;

    memset( p_packet, 0, sizeof( aw_ntp_packet_t ));

    p_packet->head       = ( LI << 30 ) | ( VN << 27 ) | ( MODE << 24 ) |
            ( STRATUM << 16) | ( POLL << 8 ) | ( PREC & 0xff );
    p_packet->rootDelay  = (1<<16);
    p_packet->origTm_f   = (1<<16);

    ntp_sys_timespec(&timespec);
    __timespec2ntptime(&timespec, &ntp_time);

//    NTP_DBG( "gw send tm:%ds,%dms\r\n", (uint32_t)timespec.tv_sec, (uint32_t)timespec.tv_nsec/ 1000000);

    p_packet->txTm_s  = ntp_time.coarse;
    p_packet->txTm_f  = ntp_time.fine;

    __send_ntp_packet_htonl(p_packet);

    return 0;
}

static int __ntp_time_get(const char *p_server_addr,
                          pfn_ntp_time_calc  ntp_tm_calc,
                          void *p_arg)
{
    int ret              = 0;
    int recv_len         = 0;
    int addr_len         = 0;
    int sockfd           = -1;
    struct sockaddr  addr_dst;
    fd_set  fds;
    struct timeval timeout;

    aw_ntp_packet_t packet;


    struct addrinfo  hints;
    struct addrinfo *p_result = NULL;

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    ret = getaddrinfo(p_server_addr, NTP_PORT_STR, &hints, &p_result);
    if( 0 != ret ) {
        NTP_DBG("getaddrinfo(%s) error:%d !\r\n",p_server_addr, ret);
        return -1;
    }

    memcpy(&addr_dst, p_result->ai_addr, sizeof(addr_dst));
    addr_len = p_result->ai_addrlen;
    freeaddrinfo(p_result);

    if((sockfd = socket(hints.ai_family, hints.ai_socktype, hints.ai_protocol)) \
        < 0) {
        NTP_DBG("create socket error!\r\n");
        return -1;
    }

#if 0
    struct sockaddr_in addr_src;
    memset(&addr_src, 0, sizeof(struct sockaddr_in));
    addr_src.sin_family = AF_INET;
    addr_src.sin_addr.s_addr = htonl(INADDR_ANY);
    addr_src.sin_port = htons(0);

    /* �󶨱���IP */
    ret = bind(sockfd, (struct sockaddr*)&addr_src, sizeof(struct sockaddr_in));
    if( 0 != ret  ) {
        NTP_DBG("bind error!\r\n");
        goto socket_err;
    }
#endif

    ret = connect(sockfd, (struct sockaddr*)&addr_dst, addr_len);
    if( 0 != ret) {
        NTP_DBG("connect error!\r\n");
        goto socket_err;
    }

    /* make data */
    __ntp_packet_make(&packet);

    send(sockfd, &packet,  sizeof(packet), 0);

    FD_ZERO(&fds);
    FD_SET(sockfd, &fds);

    timeout.tv_sec = NTP_GET_TIME_ACK_TIMEOUT_S;
    timeout.tv_usec = 0;
    ret = select(sockfd + 1, &fds, NULL, NULL, &timeout);

    if( ( ret > 0 ) &&  (0 != FD_ISSET(sockfd, &fds)) ) {

        recv_len = recvfrom(sockfd, &packet, sizeof(packet), 0, &addr_dst, (socklen_t*)&addr_len);
        if( recv_len >=  sizeof(packet)) {

            ntp_tm_calc(&packet, p_arg);

        } else {
            goto socket_err;
        }

    } else {
        NTP_DBG("recv NTP data error, ret:%d!\r\n",ret);
        goto socket_err;
    }

    shutdown(sockfd,2);
    return 0;

socket_err:

    if( sockfd >= 0 ) {
        shutdown(sockfd,2);
    }

    return -1;
}


static void __utc_time_get(aw_ntp_packet_t *p_packet, void *p_arg)
{
    struct timespec *ptimeval =(struct timespec *)p_arg;
    struct ntptime ntp_time;

    __recv_ntp_packet_ntohl(p_packet);

    ntp_time.coarse = p_packet->txTm_s;
    ntp_time.fine   = p_packet->txTm_f;
    __ntptime2timespec(&ntp_time, ptimeval);

//    NTP_DBG( "SERVER TX TIME: %s", ctime( ( const time_t* ) &ptimeval->tv_sec ) );
}

static void __dff_time_get(aw_ntp_packet_t *p_packet, void *p_arg)
{
    int64_t *p_tm_diff_ms =(int64_t *)p_arg;
    struct ntptime ntp_t1, ntp_t2, ntp_t3;
    struct timespec timespec_t1, timespec_t2, timespec_t3, timespec_t4;

    ntp_sys_timespec(&timespec_t4);

    __recv_ntp_packet_ntohl(p_packet);

    ntp_t1.coarse = p_packet->origTm_s;
    ntp_t1.fine   = p_packet->origTm_f;
    __ntptime2timespec(&ntp_t1, &timespec_t1);

    ntp_t2.coarse = p_packet->rxTm_s;
    ntp_t2.fine   = p_packet->rxTm_f;
    __ntptime2timespec(&ntp_t2, &timespec_t2);

    ntp_t3.coarse = p_packet->txTm_s;
    ntp_t3.fine   = p_packet->txTm_f;
    __ntptime2timespec(&ntp_t3, &timespec_t3);

    /* ((T2 -T1) + ( T3 -T4)) / 2 */
    *p_tm_diff_ms  = (int64_t)(((int64_t)timespec_t2.tv_sec * 1000 + (int64_t)timespec_t2.tv_nsec/ 1000000 - \
                      ((int64_t)timespec_t1.tv_sec * 1000 + (int64_t)timespec_t1.tv_nsec/ 1000000) + \
                      (int64_t)timespec_t3.tv_sec * 1000 + (int64_t)timespec_t3.tv_nsec/ 1000000 - \
                      ((int64_t)timespec_t4.tv_sec * 1000 + (int64_t)timespec_t4.tv_nsec/ 1000000)) / 2);

#if 0
    NTP_DBG( "T1:%d s,%dms\r\n", (uint32_t)timespec_t1.tv_sec, (uint32_t)timespec_t1.tv_nsec/ 1000000);
    NTP_DBG( "T2:%d s,%dms\r\n", (uint32_t)timespec_t2.tv_sec, (uint32_t)timespec_t2.tv_nsec/ 1000000);
    NTP_DBG( "T3:%d s,%dms\r\n", (uint32_t)timespec_t3.tv_sec, (uint32_t)timespec_t3.tv_nsec/ 1000000);
    NTP_DBG( "T4:%d s,%dms\r\n", (uint32_t)timespec_t4.tv_sec, (uint32_t)timespec_t4.tv_nsec/ 1000000);
    NTP_DBG( "DIFF:%dms\r\n", (int)*p_tm_diff_ms);
#endif
}

int ntp_time_get(const char *p_server_addr, struct timespec *p_timespec)
{
    if( NULL == p_server_addr ||
        NULL == p_timespec) {
        return -1;
    }

    return __ntp_time_get(p_server_addr,__utc_time_get,(void *) p_timespec);
}

int ntp_time_diff_get( const char *p_server_addr, int64_t *p_tm_diff_ms)
{
    if( NULL == p_server_addr ||
        NULL == p_tm_diff_ms) {
        return -1;
    }

    return __ntp_time_get(p_server_addr,__dff_time_get, (void *)p_tm_diff_ms);
}

void timespec_correct(struct timespec *p_timespec, int64_t error_ms)
{
    if( error_ms >= 0 ) {
        p_timespec->tv_sec  += error_ms/1000;
        p_timespec->tv_nsec += error_ms%1000 * 1000000;

        if( p_timespec->tv_nsec >= 1000000000) {
            p_timespec->tv_nsec -= 1000000000;
            p_timespec->tv_sec++;
        }

    } else {
        error_ms = 0 - error_ms;
        p_timespec->tv_sec  -= error_ms/1000;

        if( p_timespec->tv_nsec <  ( error_ms%1000 * 1000000)) {
            p_timespec->tv_sec--;
            p_timespec->tv_nsec += 1000000000;
        }

        p_timespec->tv_nsec -= error_ms%1000 * 1000000;
    }
}
/* end of file */

