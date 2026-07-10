



const keyCache = new Map<string, Promise<CryptoKey>>();

export function cachedHmacKey(secret: string): Promise<CryptoKey> {
  let promise = keyCache.get(secret);
  if (!promise) {
    promise = crypto.subtle.importKey(
      "raw",
      new TextEncoder().encode(secret),
      { name: "HMAC", hash: "SHA-256" },
      false,
      ["sign"],
    ).catch(error => {
      keyCache.delete(secret);
      throw error;
    });
    keyCache.set(secret, promise);
  }
  return promise;
}





export function constantTimeEqual(left: string, right: string): boolean {
  const a = new TextEncoder().encode(left);
  const b = new TextEncoder().encode(right);
  let diff = a.length ^ b.length;
  const length = Math.max(a.length, b.length, 1);
  for (let index = 0; index < length; index += 1) {
    diff |= (a[index % Math.max(1, a.length)] ?? 0) ^ (b[index % Math.max(1, b.length)] ?? 0);
  }
  return diff === 0;
}
