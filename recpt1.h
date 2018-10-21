#ifndef _RECPT1_H_
#define _RECPT1_H_

#define CHTYPE_SATELLITE 0  // satellite digital
#define CHTYPE_GROUND    1  // terrestrial digital
#define MAX_CH 256
#define MAX_QUEUE 8192
#define MAX_READ_SIZE (188 * 87)  // splitterが188アライメントを期待しているのでこの数字とする
#define WRITE_SIZE (1024 * 1024 * 2)
#define TRUE 1
#define FALSE 0

typedef struct _BUFSZ {
	int size;
	u_char buffer[MAX_READ_SIZE];
} BUFSZ;

typedef struct _QUEUE_T {
	unsigned int in;            // 次に入れるインデックス
	unsigned int out;           // 次に出すインデックス
	unsigned int size;          // キューのサイズ
	unsigned int num_avail;     // 満タンになると0になる
	unsigned int num_used;      // 空っぽになると0になる
	pthread_mutex_t mutex;
	pthread_cond_t cond_avail;  // データが満タンのときに待つためのcond
	pthread_cond_t cond_used;   // データが空のときに待つためのcond
	BUFSZ *buffer[1];           // バッファポインタ
} QUEUE_T;

typedef struct _ISDB_FREQ_CONV_TABLE {
	char channel[16];    // チャンネル
	int freq_no;         // 周波数番号
	unsigned int tsid;   // transport_stream_id
} ISDB_FREQ_CONV_TABLE;

typedef struct _CHANNEL_SET {
	char parm_freq[16];  // パラメータで受ける値
	int freq_no;         // 周波数番号
	int type;            // チャンネルタイプ
	unsigned int tsid;   // transport_stream_id
} CHANNEL_SET;

#endif
