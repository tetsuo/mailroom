const crypto = require('crypto')

const encodedSecret = 'Qg84W2nmW8ClkDGZBdoC7UXM0BakAhmlBmR4emc4JJji0aMHLqfdM07rn1sHN8tvGk9go-hsKJMwvtqdGm5Cjw'

const keyHex = 'deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef'

function base64UrlToBase64(base64Url) {
  return base64Url.replace(/-/g, '+').replace(/_/g, '/') + '='.repeat((4 - (base64Url.length % 4)) % 4)
}

function verifyHmac() {
  // Decode the Base64-URL encoded string
  const base64String = base64UrlToBase64(encodedSecret)
  const decoded = Buffer.from(base64String, 'base64')

  if (decoded.length !== 64) {
    console.error('Invalid input: Expected 64 bytes, got:', decoded.length)
    process.exit(1)
  }

  // Split into two parts (32 bytes secret + 32 bytes HMAC signature)
  const secret = decoded.slice(0, 32)
  const signature = decoded.slice(32)

  console.log('Secret (Hex):', secret.toString('hex'))
  console.log('Signature (Hex):', signature.toString('hex'))

  // Generate the HMAC using "/activate" (9 bytes) + secret
  const hmacKey = Buffer.from(keyHex, 'hex')
  const dataToSign = Buffer.concat([Buffer.from('/activate'), secret])

  const hmac = crypto.createHmac('sha256', hmacKey)
  hmac.update(dataToSign)
  const calculatedSignature = hmac.digest()

  console.log('Calculated Signature (Hex):', calculatedSignature.toString('hex'))

  // Compare the calculated signature with the provided signature
  const isValid = crypto.timingSafeEqual(signature, calculatedSignature)
  console.log('HMAC Verification:', isValid ? 'Valid ✅' : 'Invalid ❌')
}

verifyHmac()
