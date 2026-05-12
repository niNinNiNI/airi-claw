import { Show, type Component, type JSX } from 'solid-js';
import { t } from '../../i18n';
import { appStatus } from '../../state/config';

export const Logo: Component<{ class?: string }> = (props) => (
  <span
    class={[
      'inline-flex items-baseline gap-0',
      'text-[1.5rem] leading-none font-bold tracking-[-0.03em] select-none',
      props.class ?? '',
    ].join(' ').trim()}
  >
    <span
      class="text-[var(--color-accent)]"
      style={{
        'font-family': "'Georgia', 'Times New Roman', serif",
        'font-style': 'italic',
        'text-shadow': '0 0 2px rgba(232,54,45,0.3), 1px 1px 0 rgba(232,54,45,0.15)',
        'letter-spacing': '-0.05em',
      }}
    >
      A
    </span>
    <span
      class="text-[var(--color-accent)]"
      style={{
        'font-family': "'Impact', 'Arial Black', sans-serif",
        'font-style': 'normal',
        transform: 'skewX(-8deg)',
        display: 'inline-block',
      }}
    >
      I
    </span>
    <span
      class="text-[var(--color-accent)]"
      style={{
        'font-family': "'Georgia', 'Times New Roman', serif",
        'font-style': 'italic',
        'text-shadow': '0 0 3px rgba(232,54,45,0.4), -1px 1px 0 rgba(232,54,45,0.2)',
        'letter-spacing': '-0.1em',
        transform: 'rotate(-3deg)',
        display: 'inline-block',
      }}
    >
      R
    </span>
    <span
      class="text-[var(--color-accent)]"
      style={{
        'font-family': "'Arial Black', 'Impact', sans-serif",
        'font-style': 'normal',
        'font-weight': 900,
        'text-shadow': '1px 1px 0 rgba(232,54,45,0.25), 2px 2px 0 rgba(232,54,45,0.1)',
      }}
    >
      I
    </span>
  </span>
);

export const StatusSummary: Component<{ class?: string; compact?: boolean }> = (props) => {
  const online = () => appStatus()?.wifi_connected === true;
  const loading = () => appStatus() === null;

  return (
    <div
      class={[
        'min-w-0 flex items-center gap-2 text-[0.8rem]',
        props.compact ? 'flex-nowrap overflow-hidden' : 'flex-wrap',
        props.class ?? '',
      ].join(' ').trim()}
    >
      <span
        class={[
          'inline-flex items-center gap-2 px-3 py-1 rounded-full border font-medium min-w-0',
          loading()
            ? 'border-[var(--color-border-subtle)] text-[var(--color-text-muted)] bg-white/[0.04]'
            : online()
            ? 'border-[rgba(104,211,145,0.2)] bg-[var(--color-green-dim)] text-[var(--color-green)]'
            : 'border-[var(--color-border-subtle)] bg-white/[0.04] text-[var(--color-text-muted)]',
        ].join(' ')}
      >
        <span
          class={[
            'w-1.5 h-1.5 rounded-full',
            online() ? 'bg-[var(--color-green)] pulse-dot' : 'bg-[var(--color-text-muted)]',
          ].join(' ')}
        />
        <span class={props.compact ? 'truncate' : ''}>
          {loading()
            ? t('statusLoading')
            : online()
            ? t('statusOnline')
            : appStatus()?.ap_active
            ? t('statusApActive')
            : t('statusOffline')}
        </span>
      </span>
      <Show when={appStatus()?.ip}>
        <span class={['text-[var(--color-border-subtle)] select-none', props.compact ? 'shrink-0' : ''].join(' ')}>
          ·
        </span>
        <span class={['font-mono text-[0.78rem] text-[var(--color-text-secondary)]', props.compact ? 'truncate min-w-0' : ''].join(' ')}>
          IP: {appStatus()!.ip}
        </span>
      </Show>
      <Show when={appStatus()?.storage_base_path}>
        <span class={['text-[var(--color-border-subtle)] select-none', props.compact ? 'shrink-0' : ''].join(' ')}>
          ·
        </span>
        <span class={['font-mono text-[0.78rem] text-[var(--color-text-secondary)]', props.compact ? 'truncate min-w-0' : ''].join(' ')}>
          Storage: {appStatus()!.storage_base_path}
        </span>
      </Show>
    </div>
  );
};

type StatusBarProps = {
  leadingSlot?: JSX.Element;
  slot?: () => any;
};

export const StatusBar: Component<StatusBarProps> = (props) => {
  return (
    <header class="flex items-center justify-between gap-3 h-14 px-4 sm:px-5 border-b border-[var(--color-border-subtle)] bg-[rgba(10,11,14,0.85)] backdrop-blur-md sticky top-0 z-40">
      <div class="flex items-center gap-3 sm:gap-4 min-w-0">
        {props.leadingSlot}
        <a href="/" class="flex items-center text-[var(--color-text-primary)] no-underline">
          <Logo />
        </a>
        <div class="hidden lg:flex">
          <StatusSummary />
        </div>
      </div>
      <div class="flex items-center gap-2">{props.slot?.()}</div>
    </header>
  );
};
