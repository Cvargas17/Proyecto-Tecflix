import { useRef, useState } from 'react';
import { uploadVideo } from '../api';
import type { Visibility } from '../types';

export default function UploadView({ onUploaded }: { onUploaded: () => void }) {
  const fileRef = useRef<HTMLInputElement>(null);
  const [title, setTitle] = useState('');
  const [visibility, setVisibility] = useState<Visibility>('private');
  const [progress, setProgress] = useState<number | null>(null);
  const [error, setError] = useState('');

  const submit = async () => {
    const file = fileRef.current?.files?.[0];
    setError('');
    if (!file) { setError('Choose a video file first.'); return; }
    setProgress(0);
    try {
      await uploadVideo(file, title, visibility, setProgress);
      onUploaded();
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Upload failed');
    } finally {
      setProgress(null);
    }
  };

  return (
    <section>
      <h2>Upload a video</h2>
      <div className="form">
        <label>Video file (mp4, m4v, mov, webm, ogv, mkv)</label>
        <input ref={fileRef} type="file" accept=".mp4,.m4v,.mov,.webm,.ogv,.ogg,.mkv,video/*" />
        <label>Title</label>
        <input type="text" value={title} maxLength={120} placeholder="Defaults to the file name" onChange={(e) => setTitle(e.target.value)} />
        <label>Visibility</label>
        <select value={visibility} onChange={(e) => setVisibility(e.target.value as Visibility)}>
          <option value="private">Private (only me)</option>
          <option value="public">Public (everyone)</option>
        </select>
        <button className="btn red" disabled={progress !== null} onClick={() => void submit()}>Upload</button>
        {progress !== null && <progress value={progress} max={100} />}
        <div className="err">{error}</div>
      </div>
    </section>
  );
}
