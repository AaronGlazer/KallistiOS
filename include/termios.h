#ifndef KOS_INCLUDE_TERMIOS_H_
#define KOS_INCLUDE_TERMIOS_H_

#define NCCS 32

typedef unsigned char cc_t;
typedef unsigned char tcflag_t;

struct termios {
    /** \brief input modes */
    tcflag_t c_iflag;
    /** \brief output modes */
    tcflag_t c_oflag;
    /** \brief control modes */
    tcflag_t c_cflag;
    /** \brief local modes */
    tcflag_t c_lflag;
    /** \brief control chars */
    cc_t c_cc[NCCS];
};

#endif  // KOS_INCLUDE_TERMIOS_H_
