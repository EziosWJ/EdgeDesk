import { useEffect, useRef } from 'react';

type Props = {
  checked: boolean;
  onChange: (checked: boolean) => void;
  label: string;
};

export function SelectionCheckbox({ checked, onChange, label }: Props) {
  return (
    <input
      type="checkbox"
      checked={checked}
      aria-label={label}
      onChange={(event) => onChange(event.target.checked)}
    />
  );
}

type HeaderProps = Props & { indeterminate: boolean };

/** Select-all checkbox with the visual indeterminate state. */
export function SelectionHeaderCheckbox({ checked, indeterminate, onChange, label }: HeaderProps) {
  const ref = useRef<HTMLInputElement | null>(null);

  useEffect(() => {
    if (ref.current) ref.current.indeterminate = indeterminate;
  }, [indeterminate]);

  return (
    <input
      ref={ref}
      type="checkbox"
      checked={checked}
      aria-label={label}
      onChange={(event) => onChange(event.target.checked)}
    />
  );
}
