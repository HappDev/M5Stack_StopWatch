// Synthetic export using the public RFC 6238 SHA-1 test key. Never use for real 2FA.
export function varint(value) {
  let n=BigInt.asUintN(64,BigInt(value));const out=[];
  while(n>127n){out.push(Number(n&127n)|128);n>>=7n;}out.push(Number(n));return out;
}
export const number=(field,value)=>[...varint(field*8),...varint(value)];
export function blob(field,value){const b=typeof value==='string'?new TextEncoder().encode(value):value;return [...varint(field*8+2),...varint(b.length),...b];}
export function entry({name='Example:alice@example.com',issuer='Example',secret='12345678901234567890',algorithm=1,digits=1,type=2}={}){
  return [...blob(1,secret),...blob(2,name),...blob(3,issuer),...number(4,algorithm),...number(5,digits),...number(6,type)];
}
export const uri=bytes=>'otpauth-migration://offline?data='+encodeURIComponent(Buffer.from(bytes).toString('base64'));
export function migration(entries=[entry()],{version=1,size=1,index=0,id=42}={}){
  return uri([...entries.flatMap(e=>blob(1,e)),...number(2,version),...number(3,size),...number(4,index),...number(5,id)]);
}
