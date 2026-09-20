#ifndef AUDIO_AUDIO_TASK_H
#define AUDIO_AUDIO_TASK_H

/* オーディオ関連タスクの生成・起動 (usermain.c から呼ぶ) */
EXPORT void audio_task_start(void);

/* パススルーが実際に動いているか (usermain がトレース区間を合わせるのに使う) */
EXPORT BOOL audio_passthrough_active(void);

#endif	/* AUDIO_AUDIO_TASK_H */
