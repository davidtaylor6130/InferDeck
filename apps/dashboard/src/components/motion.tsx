import React, { useEffect, useLayoutEffect, useRef, useState } from 'react';

const useIsomorphicLayoutEffect = typeof window === 'undefined' ? useEffect : useLayoutEffect;

/**
 * Tracks the element marked data-active="true" inside a container so a single
 * highlight can glide between options instead of jumping.
 */
export function useSlidingIndicator<T extends HTMLElement>(activeKey: string) {
  const container = useRef<T>(null);
  const [box, setBox] = useState<{ x: number; y: number; width: number; height: number } | null>(null);
  const [animated, setAnimated] = useState(false);

  useIsomorphicLayoutEffect(() => {
    const node = container.current;
    if (!node) return;
    const measure = () => {
      const active = node.querySelector<HTMLElement>('[data-active="true"]');
      setBox(active ? { x: active.offsetLeft, y: active.offsetTop, width: active.offsetWidth, height: active.offsetHeight } : null);
    };
    measure();
    const frame = requestAnimationFrame(() => setAnimated(true));
    const observer = typeof ResizeObserver === 'undefined' ? null : new ResizeObserver(measure);
    observer?.observe(node);
    return () => {
      cancelAnimationFrame(frame);
      observer?.disconnect();
    };
  }, [activeKey]);

  return { container, box, animated, ready: box !== null };
}

export const SlidingIndicator: React.FC<{
  box: { x: number; y: number; width: number; height: number } | null;
  animated: boolean;
  className?: string;
}> = ({ box, animated, className = 'rounded-md bg-panel-slate shadow-card' }) => (
  <span
    aria-hidden="true"
    className={`pointer-events-none absolute left-0 top-0 ${animated ? 'transition-[transform,width,height,opacity] duration-300 ease-[cubic-bezier(0.3,1.15,0.4,1)]' : ''} ${className}`}
    style={box
      ? { width: box.width, height: box.height, transform: `translate(${box.x}px, ${box.y}px)`, opacity: 1 }
      : { opacity: 0 }}
  />
);

/** Staggered entrance for items in a list or grid. */
export const stagger = (index: number, step = 28) => ({ animationDelay: `${Math.min(index, 14) * step}ms` });
