import { useEffect, useState } from 'react';
import { api } from '../api';
import type { Video } from '../types';
import VideoCard from './VideoCard';

interface Props {
  scope: 'public' | 'mine';
  reloadKey: number;
  onOpen: (id: string) => void;
}

export default function VideoGrid({ scope, reloadKey, onOpen }: Props) {
  const [videos, setVideos] = useState<Video[] | null>(null);
  const [error, setError] = useState('');

  useEffect(() => {
    let alive = true;
    setError('');
    api.list(scope)
      .then((v) => alive && setVideos(v))
      .catch((e: Error) => alive && setError(e.message));
    return () => { alive = false; };
  }, [scope, reloadKey]);

  if (error) return <div className="empty">{error}</div>;
  if (!videos) return <div className="empty">Loading…</div>;
  if (!videos.length)
    return <div className="empty">{scope === 'mine' ? 'You have not uploaded anything yet.' : 'No public videos yet.'}</div>;
  return (
    <div className="grid">
      {videos.map((v) => <VideoCard key={v.id} video={v} onOpen={onOpen} />)}
    </div>
  );
}
