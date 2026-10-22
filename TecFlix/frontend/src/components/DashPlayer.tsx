import { useEffect, useRef, useState } from 'react';
import * as dashjs from 'dashjs';
import type { Video } from '../types';

/**
 * Plays a video with dash.js (adaptive MPEG-DASH) when the server has packaged it,
 * and falls back to the plain progressive file otherwise.
 */
export default function DashPlayer({ video }: { video: Video }) {
  const ref = useRef<HTMLVideoElement>(null);
  const [error, setError] = useState('');
  const useDash = video.dash === 'ready' && dashjs.supportsMediaSource();

  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    setError('');
    if (!useDash) {
      el.src = `/video/${video.id}`;
      return () => { el.pause(); el.removeAttribute('src'); el.load(); };
    }
    const player = dashjs.MediaPlayer().create();
    player.updateSettings({ streaming: { buffer: { fastSwitchEnabled: true } } });
    player.on('error', (e: unknown) => {
      const err = e as { error?: { message?: string } };
      setError(err.error?.message ?? 'Playback error');
    });
    player.initialize(el, `/dash/${video.id}/manifest.mpd`, false);
    return () => player.destroy();
  }, [video.id, useDash]);

  const note: Record<Video['dash'], string> = {
    ready: 'Streaming with MPEG-DASH (dash.js)',
    processing: 'Preparing adaptive stream… playing the original file meanwhile',
    failed: 'DASH packaging failed — playing the original file',
    unavailable: 'DASH unavailable (ffmpeg not installed on the server) — playing the original file',
  };

  return (
    <>
      <video ref={ref} className="player" controls />
      <div className="dim player-note">{error ? `⚠ ${error}` : note[video.dash]}</div>
    </>
  );
}
