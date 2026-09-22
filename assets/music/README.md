# 声音生成

## 声音素材

音效文件来自 https://freesound.org/

人声生成来自 https://edge-tts.com/

使用下面的命令合成`任务完成`的声音：

```shell
ffmpeg -i taskdone_human.mp3 -i taskdone_voice.mp3 \
  -filter_complex "[0:a]adelay=800|800[a];[a][1:a]amix=inputs=2:duration=longest" \
  -ar 16000 -ac 1 -sample_fmt s16 taskdone.wav
```
