// @vitest-environment happy-dom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { beforeEach, describe, expect, it } from 'vitest';
import { DonutChart } from './DonutChart';

describe('DonutChart', () => {
  let container: HTMLDivElement;
  let root: Root;

  beforeEach(() => {
    document.body.innerHTML = '';
    container = document.createElement('div');
    document.body.appendChild(container);
    root = createRoot(container);
  });

  const render = (percent: number | null) => {
    act(() => {
      root.render(<DonutChart percent={percent} />);
    });
  };

  it('renders the available share in green and the used share in the primary color', () => {
    render(4);
    expect(container.querySelector('.ed-donut-available')?.getAttribute('stroke')).toBe('var(--ed-accent)');
    expect(container.querySelector('.ed-donut-used')?.getAttribute('stroke')).toBe('var(--ed-primary)');
    expect(container.querySelector('.ed-donut-center strong')?.textContent).toBe('4%');
  });

  it('keeps a neutral track when the usage is unavailable', () => {
    render(null);
    expect(container.querySelector('.ed-donut-available')?.getAttribute('stroke')).toBe('var(--ed-donut-track)');
    expect(container.querySelector('.ed-donut-used')).toBeNull();
    expect(container.querySelector('.ed-donut-center strong')?.textContent).toBe('—');
  });
});
