/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package android.hardware.security.keymint;

import android.hardware.security.keymint.MacedPublicKey;
import android.hardware.security.keymint.ProtectedData;

/**
 * An IRemotelyProvisionedComponent is a secure-side component for which certificates can be
 * remotely provisioned. It provides an interface for generating asymmetric key pairs and then
 * creating a CertificateRequest that contains the generated public keys, plus other information to
 * authenticate the request origin. The CertificateRequest can be sent to a server, which can
 * validate the request and create certificates.
 *
 * This interface does not provide any way to use the generated and certified key pairs. It's
 * intended to be implemented by a HAL service that does other things with keys (e.g. Keymint).
 *
 * The root of trust for secure provisioning is something called the "Boot Certificate Chain", or
 * BCC. The BCC is a chain of public key certificates, represented as COSE_Sign1 objects containing
 * COSE_Key representations of the public keys. The "root" of the BCC is a self-signed certificate
 * for a device-unique public key, denoted DK_pub. All public keys in the BCC are device-unique. The
 * public key from each certificate in the chain is used to sign the next certificate in the
 * chain. The final, "leaf" certificate contains a public key, denoted KM_pub, whose corresponding
 * private key, denoted KM_priv, is available for use by the IRemotelyProvisionedComponent.
 *
 * BCC Design
 * ==========
 *
 * The BCC is designed to mirror the boot stages of a device, and to prove the content and integrity
 * of each firmware image. In a proper BCC, each boot stage hashes its own private key with the code
 * and any relevant configuration parameters of the next stage to produce a key pair for the next
 * stage. Each stage also uses its own private key to sign the public key of the next stage,
 * including in the certificate the hash of the next firmware stage, then loads the next stage,
 * passing the private key and certificate to it in a manner that does not leak the private key to
 * later boot stages. The BCC root key pair is generated by immutable code (e.g. ROM), from a
 * device-unique secret. After the device-unique secret is used, it must be made unavailable to any
 * later boot stage.
 *
 * In this way, booting the device incrementally builds a certificate chain that (a) identifies and
 * validates the integrity of every stage and (b) contains a set of public keys that correspond to
 * private keys, one known to each stage. Any stage can compute the secrets of all later stages
 * (given the necessary input), but no stage can compute the secret of any preceding stage. Updating
 * the firmware or configuration of any stage changes the key pair of that stage, and of all
 * subsequent stages, and no attacker who compromised the previous version of the updated firmware
 * can know or predict the post-update key pairs.
 *
 * The first BCC certificate is special because its contained public key, DK_pub, will never change,
 * making it a permanent, device-unique identifier. Although the remaining keys in the BCC are also
 * device-unique, they are not necessarily permanent, since they can change when the device software
 * is updated.
 *
 * When the provisioning server receives a message signed by KM_priv and containing a BCC that
 * chains from DK_pub to KM_pub, it can be certain that (barring vulnerabilities in some boot
 * stage), the CertificateRequest came from the device associated with DK_pub, running the specific
 * software identified by the certificates in the BCC. If the server has some mechanism for knowing
 * which the DK_pub values of "valid" devices, it can determine whether signing certificates is
 * appropriate.
 *
 * Degenerate BCCs
 * ===============
 *
 * While a proper BCC, as described above, reflects the complete boot sequence from boot ROM to the
 * secure area image of the IRemotelyProvisionedComponent, it's also possible to use a "degenerate"
 * BCC which consists only of a single, self-signed certificate containing the public key of a
 * hardware-bound key pair. This is an appopriate solution for devices which haven't implemented
 * everything necessary to produce a proper BCC, but can derive a unique key pair in the secure
 * area.  In this degenerate case, DK_pub is the same as KM_pub.
 *
 * BCC Privacy
 * ===========
 *
 * Because the BCC constitutes an unspoofable, device-unique identifier, special care is taken to
 * prevent its availability to entities who may wish to track devices. Two precautions are taken:
 *
 * 1.  The BCC is never exported from the IRemotelyProvisionedComponent except in encrypted
 *     form. The portion of the CertificateRequest that contains the BCC is encrypted using an
 *     Endpoint Encryption Key (EEK).  The EEK is provided in the form of a certificate chain whose
 *     root must be pre-provisioned into the secure area (hardcoding the roots into the secure area
 *     firmware image is a recommended approach). Multiple roots may be provisioned. If the provided
 *     EEK does not chain back to this already-known root, the IRemotelyProvisionedComponent must
 *     reject it.
 *
 * 2.  Precaution 1 above ensures that only an entity with a valid EEK private key can decrypt the
 *     BCC. To make it feasible to build a provisioning server which cannot use the BCC to track
 *     devices, the CertificateRequest is structured so that the server can be partitioned into two
 *     components.  The "decrypter" decrypts the BCC, verifies DK_pub and the device's right to
 *     receive provisioned certificates, but does not see the public keys to be signed or the
 *     resulting certificates.  The "certifier" gets informed of the results of the decrypter's
 *     validation and sees the public keys to be signed and resulting certificates, but does not see
 *     the BCC.
 *
 * Test Mode
 * =========
 *
 * The IRemotelyProvisionedComponent supports a test mode, allowing the generation of test key pairs
 * and test CertificateRequests. Test keys/requests are annotated as such, and the BCC used for test
 * CertificateRequests must contain freshly-generated keys, not the real BCC key pairs.
 */
@VintfStability
interface IRemotelyProvisionedComponent {
    const int STATUS_FAILED = 1;
    const int STATUS_INVALID_MAC = 2;
    const int STATUS_PRODUCTION_KEY_IN_TEST_REQUEST = 3;
    const int STATUS_TEST_KEY_IN_PRODUCTION_REQUEST = 4;
    const int STATUS_INVALID_EEK = 5;

    /**
     * generateKeyPair generates a new ECDSA P-256 key pair that can be certified.  Note that this
     * method only generates ECDSA P-256 key pairs, but the interface can be extended to add methods
     * for generating keys for other algorithms, if necessary.
     *
     * @param in boolean testMode indicates whether the generated key is for testing only. Test keys
     *        are marked (see the definition of PublicKey in the MacedPublicKey structure) to
     *        prevent them from being confused with production keys.
     *
     * @param out MacedPublicKey macedPublicKey contains the public key of the generated key pair,
     *        MACed so that generateCertificateRequest can easily verify, without the
     *        privateKeyHandle, that the contained public key is for remote certification.
     *
     * @return data representing a handle to the private key. The format is implementation-defined,
     *         but note that specific services may define a required format.
     */
    byte[] generateEcdsaP256KeyPair(in boolean testMode, out MacedPublicKey macedPublicKey);

    /**
     * generateCertificateRequest creates a certificate request to be sent to the provisioning
     * server.
     *
     * @param in boolean testMode indicates whether the generated certificate request is for testing
     *        only.
     *
     * @param in MacedPublicKey[] keysToSign contains the set of keys to certify. The
     *        IRemotelyProvisionedComponent must validate the MACs on each key.  If any entry in the
     *        array lacks a valid MAC, the method must return STATUS_INVALID_MAC.
     *
     *        If testMode is true, the keysToCertify array must contain only keys flagged as test
     *        keys. Otherwise, the method must return STATUS_PRODUCTION_KEY_IN_TEST_REQUEST.
     *
     *        If testMode is false, the keysToCertify array must not contain any keys flagged as
     *        test keys. Otherwise, the method must return STATUS_TEST_KEY_IN_PRODUCTION_REQUEST.
     *
     * @param in endpointEncryptionKey contains an X25519 public key which will be used to encrypt
     *        the BCC. For flexibility, this is represented as a certificate chain, represented as a
     *        CBOR array of COSE_Sign1 objects, ordered from root to leaf. The leaf contains the
     *        X25519 encryption key, each other element is an Ed25519 key signing the next in the
     *        chain. The root is self-signed.
     *
     *            EekChain = [ + SignedSignatureKey, SignedEek ]
     *
     *            SignedSignatureKey = [              // COSE_Sign1
     *                protected: bstr .cbor {
     *                    1 : -8,                     // Algorithm : EdDSA
     *                },
     *                unprotected: { },
     *                payload: bstr .cbor SignatureKey,
     *                signature: bstr PureEd25519(.cbor SignatureKeySignatureInput)
     *            ]
     *
     *            SignatureKey = {                    // COSE_Key
     *                 1 : 1,                         // Key type : Octet Key Pair
     *                 3 : -8,                        // Algorithm : EdDSA
     *                 -1 : 6,                        // Curve : Ed25519
     *                 -2 : bstr                      // Ed25519 public key
     *            }
     *
     *            SignatureKeySignatureInput = [
     *                context: "Signature1",
     *                body_protected: bstr .cbor {
     *                    1 : -8,                     // Algorithm : EdDSA
     *                },
     *                external_aad: bstr .size 0,
     *                payload: bstr .cbor SignatureKey
     *            ]
     *
     *            SignedEek = [                       // COSE_Sign1
     *                protected: bstr .cbor {
     *                    1 : -8,                     // Algorithm : EdDSA
     *                },
     *                unprotected: { },
     *                payload: bstr .cbor Eek,
     *                signature: bstr PureEd25519(.cbor EekSignatureInput)
     *            ]
     *
     *            Eek = {                             // COSE_Key
     *                1 : 1,                          // Key type : Octet Key Pair
     *                2 : bstr                        // KID : EEK ID
     *                3 : -25,                        // Algorithm : ECDH-ES + HKDF-256
     *                -1 : 4,                         // Curve : X25519
     *                -2 : bstr                       // Ed25519 public key
     *            }
     *
     *            EekSignatureInput = [
     *                context: "Signature1",
     *                body_protected: bstr .cbor {
     *                    1 : -8,                     // Algorithm : EdDSA
     *                },
     *                external_aad: bstr .size 0,
     *                payload: bstr .cbor Eek
     *            ]
     *
     *        If the contents of endpointEncryptionKey do not match the SignedEek structure above,
     *        the method must return STATUS_INVALID_EEK.
     *
     *        If testMode is true, the method must ignore the length and content of the signatures
     *        in the chain, which implies that it must not attempt to validate the signature.
     *
     *        If testMode is false, the method must validate the chain signatures, and must verify
     *        that the public key in the root certifictate is in its pre-configured set of
     *        authorized EEK root keys. If the public key is not in the database, or if signature
     *        verification fails, the method must return STATUS_INVALID_EEK.
     *
     * @param in challenge contains a byte string from the provisioning server that must be signed
     *        by the secure area. See the description of the 'signature' output parameter for
     *        details.
     *
     * @param out keysToSignMac contains the MAC of KeysToSign in the CertificateRequest
     *        structure. Specifically, it contains:
     *
     *            HMAC-256(EK_mac, .cbor KeysToMacStructure)
     *
     *        Where EK_mac is an ephemeral MAC key, found in ProtectedData (see below).  The MACed
     *        data is the "tag" field of a COSE_Mac0 structure like:
     *
     *            MacedKeys = [                            // COSE_Mac0
     *                protected : bstr .cbor {
     *                    1 : 5,                           // Algorithm : HMAC-256
     *                },
     *                unprotected : { },
     *                // Payload is PublicKeys from keysToSign argument, in provided order.
     *                payload: bstr .cbor [ * PublicKey ],
     *                tag: bstr
     *           ]
     *
     *            KeysToMacStructure = [
     *                context : "MAC0",
     *                protected : bstr .cbor { 1 : 5 },    // Algorithm : HMAC-256
     *                external_aad : bstr .size 0,
     *                // Payload is PublicKeys from keysToSign argument, in provided order.
     *                payload : bstr .cbor [ * PublicKey ]
     *            ]
     *
     * @param out ProtectedData contains the encrypted BCC and the ephemeral MAC key used to
     *        authenticate the keysToSign (see keysToSignMac output argument).
     */
    void generateCertificateRequest(in boolean testMode, in MacedPublicKey[] keysToSign,
            in byte[] endpointEncryptionCertChain, in byte[] challenge, out byte[] keysToSignMac,
            out ProtectedData protectedData);
}
