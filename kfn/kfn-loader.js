const ENTRY_TYPES = {
  SONGTEXT: 1,
  MUSIC: 2,
  IMAGE: 3,
  FONT: 4,
  VIDEO: 5
};

class KFNDumper {
  constructor(arrayBuffer) {
    this.buffer = new Uint8Array(arrayBuffer);
    this.offset = 0;
    this.aesKey = null;
  }

  readByte() {
    if (this.offset >= this.buffer.length) {
      throw new Error('Unexpected end of file');
    }
    return this.buffer[this.offset++];
  }

  readDword() {
    const b1 = this.readByte();
    const b2 = this.readByte();
    const b3 = this.readByte();
    const b4 = this.readByte();
    return b1 | (b2 << 8) | (b3 << 16) | (b4 << 24);
  }

  readBytes(length) {
    if (this.offset + length > this.buffer.length) {
      throw new Error(`Cannot read ${length} bytes: not enough data`);
    }
    const data = this.buffer.slice(this.offset, this.offset + length);
    this.offset += length;
    return data;
  }

  readString(length) {
    const bytes = this.readBytes(length);
    const decoder = new TextDecoder('utf-8');
    return decoder.decode(bytes);
  }

  list() {
    this.offset = 0;
    const entries = [];

    const sig = this.readString(4);
    if (sig !== 'KFNB') {
      throw new Error('Invalid KFN file signature');
    }

    while (true) {
      const headerSig = this.readString(4);
      const type = this.readByte();
      const lenOrValue = this.readDword();
      let buf = null;

      if (type === 2) {
        buf = this.readBytes(lenOrValue);
      }

      if (headerSig === 'FLID' && buf !== null) {
        this.aesKey = buf;
      }

      if (headerSig === 'ENDH') {
        break;
      }
    }

    const numFiles = this.readDword();

    for (let i = 0; i < numFiles; i++) {
      const filenameLen = this.readDword();
      const filename = this.readString(filenameLen);

      const entry = {
        filename,
        type: this.readDword(),
        lengthOut: this.readDword(),
        offset: this.readDword(),
        lengthIn: this.readDword(),
        flags: this.readDword()
      };

      entries.push(entry);
    }

    const dirEnd = this.offset;
    entries.forEach(entry => {
      entry.offset += dirEnd;
    });

    return entries;
  }

  async extract(entry) {
    if ((entry.flags & 0x01) && !this.aesKey) {
      throw new Error('Encryption key is unknown');
    }

    const data = this.buffer.slice(entry.offset, entry.offset + entry.lengthIn);

    if (entry.flags & 0x01) {
      return await this.decryptAES128ECB(data, this.aesKey, entry.lengthOut);
    }

    return data.slice(0, entry.lengthOut);
  }

  async decryptAES128ECB(encryptedData, key, outputLength) {
    if (!window.CryptoJS) {
      throw new Error('CryptoJS library not loaded');
    }

    const keyWordArray = window.CryptoJS.enc.Hex.parse(
      Array.from(key).map(b => b.toString(16).padStart(2, '0')).join('')
    );
    
    const encryptedWordArray = window.CryptoJS.enc.Hex.parse(
      Array.from(encryptedData).map(b => b.toString(16).padStart(2, '0')).join('')
    );

    const decrypted = window.CryptoJS.AES.decrypt(
      { ciphertext: encryptedWordArray },
      keyWordArray,
      { mode: window.CryptoJS.mode.ECB, padding: window.CryptoJS.pad.NoPadding }
    );

    const decryptedBytes = new Uint8Array(decrypted.sigBytes);
    for (let i = 0; i < decrypted.sigBytes; i++) {
      decryptedBytes[i] = (decrypted.words[i >>> 2] >>> (24 - (i % 4) * 8)) & 0xff;
    }

    return decryptedBytes.slice(0, outputLength);
  }

  async extractAll() {
    const entries = this.list();
    const result = {};

    for (const entry of entries) {
      try {
        result[entry.filename] = {
          data: await this.extract(entry),
          type: entry.type,
          typeName: Object.keys(ENTRY_TYPES).find(
            key => ENTRY_TYPES[key] === entry.type
          ) || 'UNKNOWN',
          encrypted: (entry.flags & 0x01) !== 0,
          metadata: entry
        };
      } catch (error) {
        result[entry.filename] = {
          error: error.message,
          metadata: entry
        };
      }
    }

    return result;
  }
}
