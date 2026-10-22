import { useCallback, useEffect, useState } from 'react';
import { api } from '../api';
import { fmtDate, fmtDuration, fmtResolution, fmtSize } from '../format';
import type { Video, Visibility } from '../types';
import DashPlayer from './DashPlayer';
import Modal from './Modal';

interface Props {
  videoId: string;
  onClose: () => void;
}

export default function PlayerModal({ videoId, onClose }: Props) {
  const [video, setVideo] = useState<Video | null>(null);
  const [error, setError] = useState('');
  const [title, setTitle] = useState('');
  const [visibility, setVisibility] = useState<Visibility>('private');

  const apply = useCallback((v: Video) => {
    setVideo(v);
    setTitle(v.title);
    setVisibility(v.visibility);
  }, []);

  useEffect(() => {
    api.get(videoId).then(apply).catch((e: Error) => setError(e.message));
  }, [videoId, apply]);

  // While the server is packaging the DASH stream, poll until it is ready.
  const processing = video?.dash === 'processing';
  useEffect(() => {
    if (!processing) return;
    const t = setInterval(() => {
      api.get(videoId).then((v) => setVideo((old) => (old && v.dash !== old.dash ? { ...old, dash: v.dash } : old))).catch(() => {});
    }, 3000);
    return () => clearInterval(t);
  }, [processing, videoId]);

  const update = async (fields: { title?: string; visibility?: Visibility }) => {
    try {
      const v = await api.update(videoId, fields);
      setVideo((old) => (old ? { ...v, dash: old.dash } : v));
      setTitle(v.title);
      setVisibility(v.visibility);
    } catch (e) {
      alert(e instanceof Error ? e.message : 'Update failed');
    }
  };

  const remove = async () => {
    if (!video || !confirm(`Delete "${video.title}" permanently?`)) return;
    try {
      await api.remove(videoId);
      onClose();
    } catch (e) {
      alert(e instanceof Error ? e.message : 'Delete failed');
    }
  };

  if (error) return <Modal onClose={onClose}><div className="err">{error}</div></Modal>;
  if (!video) return <Modal onClose={onClose}><div className="empty">Loading…</div></Modal>;

  const m = video.meta;
  const rows: [string, string][] = [
    ['File name', m.filename],
    ['File size', fmtSize(m.size)],
    ['Format', m.mime],
    ['Duration', fmtDuration(m.duration)],
    ['Resolution', fmtResolution(m.width, m.height)],
    ['Video codec', m.vcodec || 'Unknown'],
    ['Audio codec', m.acodec || 'None / unknown'],
    ['Container brand', m.brand || 'Unknown'],
    ['Tracks', m.tracks ? String(m.tracks) : 'Unknown'],
    ['Average bitrate', m.bitrate ? `${(m.bitrate / 1e6).toFixed(2)} Mbit/s` : 'Unknown'],
    ['Media created', fmtDate(m.created)],
    ['Uploaded', fmtDate(video.uploaded)],
    ['Visibility', video.visibility],
    ['Streaming', video.dash],
  ];

  return (
    <Modal onClose={onClose}>
      <DashPlayer key={video.id} video={video} />
      <h2 style={{ marginTop: 14 }}>{video.title}</h2>
      <div className="dim">Uploaded by {video.owner} on {fmtDate(video.uploaded)}</div>

      {video.isOwner && (
        <>
          <div className="row">
            <input type="text" value={title} maxLength={120} onChange={(e) => setTitle(e.target.value)} />
            <button className="btn" onClick={() => void update({ title })}>Rename</button>
          </div>
          <div className="row">
            <label>Visibility</label>
            <select value={visibility} onChange={(e) => setVisibility(e.target.value as Visibility)}>
              <option value="private">Private</option>
              <option value="public">Public</option>
            </select>
            <button className="btn" onClick={() => void update({ visibility })}>Apply</button>
            <button className="btn danger" onClick={() => void remove()}>Delete video</button>
          </div>
        </>
      )}

      <h3>Metadata</h3>
      <table>
        <tbody>
          {rows.map(([k, v]) => (
            <tr key={k}><td>{k}</td><td>{v}</td></tr>
          ))}
        </tbody>
      </table>
    </Modal>
  );
}
