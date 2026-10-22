import { useEffect, type ReactNode } from 'react';

interface Props {
  onClose: () => void;
  small?: boolean;
  children: ReactNode;
}

export default function Modal({ onClose, small, children }: Props) {
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => e.key === 'Escape' && onClose();
    document.addEventListener('keydown', onKey);
    return () => document.removeEventListener('keydown', onKey);
  }, [onClose]);

  return (
    <div className="modal open" onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div className={'box' + (small ? ' small' : '')}>
        <button className="close" onClick={onClose} aria-label="Close">&times;</button>
        {children}
      </div>
    </div>
  );
}
