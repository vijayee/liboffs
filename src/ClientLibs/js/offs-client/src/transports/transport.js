/**
 * @typedef {(type: number, bytes: Uint8Array) => void} MessageHandler
 */

/**
 * @typedef {Object} ClientTransport
 * @property {(url: string, apiKey?: string, options?: any) => Promise<void>} connect
 * @property {() => void} disconnect
 * @property {(bytes: Uint8Array) => void} send
 * @property {(handler: MessageHandler) => void} setMessageHandler
 * @property {() => boolean} isConnected
 */

export {};
