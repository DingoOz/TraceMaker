// WebSocket client: reconnects with backoff and hands messages to the app once per animation frame.
import type { Message } from './protocol';

export type ConnState = 'connecting' | 'open' | 'closed';

export class Connection {
  private ws: WebSocket | null = null;
  private queue: Message[] = [];
  private retry = 0;
  private timer = 0;
  private bytes = 0;
  state: ConnState = 'connecting';
  onState: (s: ConnState) => void = () => {};

  constructor(private readonly url: string) {
    this.open();
  }

  static defaultUrl(): string {
    const q = new URLSearchParams(location.search).get('ws');
    if (q) return q;
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    return `${proto}//${location.host}/ws`;
  }

  private setState(s: ConnState) {
    this.state = s;
    this.onState(s);
  }

  private open() {
    this.setState('connecting');
    let ws: WebSocket;
    try {
      ws = new WebSocket(this.url);
    } catch {
      this.schedule();
      return;
    }
    this.ws = ws;
    ws.onopen = () => {
      this.retry = 0;
      this.setState('open');
    };
    ws.onmessage = (ev) => {
      if (typeof ev.data !== 'string') return;
      this.bytes += ev.data.length;
      try {
        this.queue.push(JSON.parse(ev.data) as Message);
      } catch {
        // malformed message: ignore, the stream continues
      }
    };
    ws.onclose = () => {
      this.ws = null;
      this.setState('closed');
      this.schedule();
    };
    ws.onerror = () => ws.close();
  }

  private schedule() {
    clearTimeout(this.timer);
    const delay = Math.min(5000, 400 * 2 ** this.retry++);
    this.timer = window.setTimeout(() => this.open(), delay);
  }

  /** All messages received since the last call, in order. */
  drain(): Message[] {
    const q = this.queue;
    this.queue = [];
    return q;
  }

  takeBytes(): number {
    const b = this.bytes;
    this.bytes = 0;
    return b;
  }
}
