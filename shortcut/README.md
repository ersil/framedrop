# Send to Photo Frame (iOS Shortcut)

Sends photos from the iPhone share sheet to the ESP32-S3 photo frame uploader
at `http://photoframe.local`. Each send is one batch, so the frame re-reads
its card once after the last photo instead of once per photo.

## Files

| File | What it is |
|---|---|
| `Send to Photo Frame.shortcut` | Signed (anyone can import) copy, made with `shortcuts sign --mode anyone`. Install this one. |
| `Send to Photo Frame.plist` | The same Shortcut as readable XML, unsigned. It holds no password. |

## Install

1. AirDrop `Send to Photo Frame.shortcut` to the iPhone, or open it from
   iCloud Drive or Files, and tap Add Shortcut.
2. Shortcuts asks two import questions. Leave the address as
   `photoframe.local`. For the password, enter the web password the
   firmware was built with (`PHOTOFRAME_WEB_PASSWORD`).
   The Shortcut stores it in a Text action on the phone, so the file in this
   repo never contains it.
3. To change the password later, open the Shortcut in the Shortcuts app and
   edit the second Text action (the one near the top that holds the password).

iOS asks once for Local Network access the first time the Shortcut talks to
`photoframe.local`, and the Shortcuts app may ask to allow the Shortcut to
send data to that address. Accept both.

## Use

In Photos or Files, select one or more images, tap Share, then choose
Send to Photo Frame. A notification reports `Sent N photos to the frame`, with
a failure count added if any upload did not return `"ok":true`.

## What it does

1. Builds `Authorization: Basic base64(admin:password)`.
2. `POST /api/batch/begin`. The frame keeps playing; it is not told about
   the new files until the batch ends.
3. For each image: convert to JPEG; read its width and height and resize to a
   longest edge of 1920 px only if either is larger than 1920 (smaller images
   are uploaded as converted); name it `YYYYMMDD_HHMMSS_<nnn>.jpg` (the
   timestamp is taken once per send and `<nnn>` is the position in the
   selection, zero padded to 3 digits, for example
   `20261013_093005_001.jpg`); and `POST /api/upload?path=/` as
   `multipart/form-data` with the file in the field `file`. A response that
   contains `"ok":true` counts as sent; any other response counts as failed.
4. `POST /api/batch/end`. The device signals the frame that the card changed,
   and the frame picks up the new photos once.
5. Shows the notification.

## Limits

- The username is `admin`, fixed in the Credentials action. After a username
  change on the Login tab, edit that action or every upload fails with 401.
- The phone must be on the home Wi-Fi (or on a VPN that routes to
  it) and must be able to resolve `photoframe.local` through mDNS. If that
  fails, replace the address in the first Text action with the device's IP.
- HTTP errors from the device (wrong password, card busy) are counted and the
  batch still ends. A network-level failure, such as the phone leaving Wi-Fi
  mid-send, stops the Shortcut because Shortcuts has no try/catch. The device
  then closes the batch on its own after 10 minutes without a write, and the
  frame picks up whatever arrived.
- The index is padded to 3 digits, so names sort correctly within a send of up
  to 999 photos. Photos from different sends sort by timestamp.
- Two sends started within the same second reuse timestamps and can overwrite
  each other's `_001`, `_002` files. The web page adds a random suffix to its
  names, so it cannot collide with the Shortcut or another phone.
