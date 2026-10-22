import type { Video } from '../types';
import { fmtDuration, fmtSize } from '../format';

export default function VideoCard({ video, onOpen }: { video: Video; onOpen: (id: string) => void }) {
  return (
    <div className="card" onClick={() => onOpen(video.id)}>
      <video src={`/video/${video.id}#t=0.5`} preload="metadata" muted />
      <div className="info">
        <div className="t">
          {video.title}
          <span className={`badge ${video.visibility}`}>{video.visibility}</span>
        </div>
        <div className="dim">
          {video.owner} · {fmtDuration(video.meta.duration)} · {fmtSize(video.meta.size)}
        </div>
      </div>
    </div>
  );
}
