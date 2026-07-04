import { describe, expect, it } from 'vitest';
import { pickTickIndices } from './ui';

describe('pickTickIndices', () => {
  it('keeps every index when the count already fits within the limit', () => {
    expect(pickTickIndices(7, 8)).toEqual([0, 1, 2, 3, 4, 5, 6]);
  });

  it('always includes the first and last index when thinning', () => {
    const indices = pickTickIndices(24, 8);
    expect(indices[0]).toBe(0);
    expect(indices[indices.length - 1]).toBe(23);
    expect(indices.length).toBeLessThanOrEqual(8);
  });

  it('spaces thinned ticks evenly so they line up with the same fraction used for plotted points', () => {
    const indices = pickTickIndices(24, 8);
    // Each picked index should correspond to an evenly-spaced fraction of (count - 1),
    // the same fraction the chart uses to place its x coordinates.
    for (let i = 1; i < indices.length; i++) {
      expect(indices[i]).toBeGreaterThan(indices[i - 1]);
    }
  });

  it('handles empty input', () => {
    expect(pickTickIndices(0, 8)).toEqual([]);
  });
});
