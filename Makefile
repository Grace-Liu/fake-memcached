CC = gcc
#CFLAGS = -Wall -g -DINFO -DDBGERR
CFLAGS = -DNDEBUG -O3 -DINFO -DDBGERR

TARGET = mtcp_simucached

UTIL_FLD = ../../mtcp/src
UTIL_INC = ${UTIL_FLD}/include

EPSERVER_OBJS = mtcp_simucached.o 

MTCP_FLD = ../../mtcp/lib
MTCP_INC = ../../mtcp/include
MTCP_LIB = ${MTCP_FLD}/libmtcp.a

PS_FLD = ../../io_engine/lib
PS_INC = ../../io_engine/include

INC = -I./include/ -I${UTIL_INC} -I${MTCP_INC} -I${PS_INC}
LIBS = -lnuma -lmtcp -lps -lpthread -lrt
LIB = -L${PS_FLD} -L${MTCP_FLD}

all: mtcp_simucached

%.o: %.c
	${CC} -c ${CFLAGS} ${INC} -o $@ $<

mtcp_simucached: ${EPSERVER_OBJS} ${MTCP_LIB}
	${CC} -o mtcp_simucached ${EPSERVER_OBJS} ${LIB} ${LIBS}


clean:
	rm -f *~ *.o ${TARGET}
