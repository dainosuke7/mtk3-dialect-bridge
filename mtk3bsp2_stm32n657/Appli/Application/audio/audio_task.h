#ifndef AUDIO_AUDIO_TASK_H
#define AUDIO_AUDIO_TASK_H

/* オーディオ関連タスクの生成・起動 (usermain.c から呼ぶ) */
EXPORT void audio_task_start(void);

/* パススルーが実際に動いているか (usermain がトレース区間を合わせるのに使う) */
EXPORT BOOL audio_passthrough_active(void);

/* パススルーの累計 under / over / late (音声タスクのログの under= over= late= と同じ値) */
EXPORT void audio_pt_counts(UW *under, UW *over, UW *late);

#endif	/* AUDIO_AUDIO_TASK_H */
