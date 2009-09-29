/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2006 Tatsuhiro Tsujikawa
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */
#include "BtPieceMessage.h"

#include <cstring>
#include <cstdlib>
#include <cassert>

#include "bittorrent_helper.h"
#include "Util.h"
#include "message.h"
#include "DlAbortEx.h"
#include "MessageDigestHelper.h"
#include "DiskAdaptor.h"
#include "Logger.h"
#include "Peer.h"
#include "Piece.h"
#include "PieceStorage.h"
#include "BtMessageDispatcher.h"
#include "BtMessageFactory.h"
#include "BtRequestFactory.h"
#include "PeerConnection.h"
#include "StringFormat.h"
#include "DownloadContext.h"

namespace aria2 {

const std::string BtPieceMessage::NAME("piece");

void BtPieceMessage::setBlock(const unsigned char* block, size_t blockLength) {
  delete [] this->block;
  this->blockLength = blockLength;
  this->block = new unsigned char[this->blockLength];
  memcpy(this->block, block, this->blockLength);
}

BtPieceMessageHandle BtPieceMessage::create(const unsigned char* data, size_t dataLength) {
  bittorrent::assertPayloadLengthGreater(9, dataLength, NAME);
  bittorrent::assertID(ID, data, NAME);
  BtPieceMessageHandle message(new BtPieceMessage());
  message->setIndex(bittorrent::getIntParam(data, 1));
  message->setBegin(bittorrent::getIntParam(data, 5));
  message->setBlock(data+9, dataLength-9);
  return message;
}

void BtPieceMessage::doReceivedAction() {
  RequestSlot slot = dispatcher->getOutstandingRequest(index,
						       begin,
						       blockLength);
  peer->updateDownloadLength(blockLength);
  if(!RequestSlot::isNull(slot)) {
    peer->snubbing(false);
    peer->updateLatency(slot.getLatencyInMillis());
    PieceHandle piece = pieceStorage->getPiece(index);
    off_t offset = (off_t)index*_downloadContext->getPieceLength()+begin;
    logger->debug(MSG_PIECE_RECEIVED,
		  cuid, index, begin, blockLength, offset, slot.getBlockIndex());
    pieceStorage->getDiskAdaptor()->writeData(block, blockLength, offset);
    piece->completeBlock(slot.getBlockIndex());
    logger->debug(MSG_PIECE_BITFIELD, cuid,
		  Util::toHex(piece->getBitfield(),
			      piece->getBitfieldLength()).c_str());
    piece->updateHash(begin, block, blockLength);
    dispatcher->removeOutstandingRequest(slot);
    if(piece->pieceComplete()) {
      if(checkPieceHash(piece)) {
	onNewPiece(piece);
      } else {
	onWrongPiece(piece);
      }
    }
  } else {
    logger->debug("CUID#%d - RequestSlot not found, index=%d, begin=%d",
		  cuid, index, begin);
  }
}

size_t BtPieceMessage::MESSAGE_HEADER_LENGTH = 13;

const unsigned char* BtPieceMessage::getMessageHeader() {
  if(!msgHeader) {
    /**
     * len --- 9+blockLength, 4bytes
     * id --- 7, 1byte
     * index --- index, 4bytes
     * begin --- begin, 4bytes
     * total: 13bytes
     */
    msgHeader = new unsigned char[MESSAGE_HEADER_LENGTH];
    bittorrent::createPeerMessageString(msgHeader, MESSAGE_HEADER_LENGTH,
					9+blockLength, ID);
    bittorrent::setIntParam(&msgHeader[5], index);
    bittorrent::setIntParam(&msgHeader[9], begin);
  }
  return msgHeader;
}

size_t BtPieceMessage::getMessageHeaderLength() {
  return MESSAGE_HEADER_LENGTH;
}

void BtPieceMessage::send() {
  if(invalidate) {
    return;
  }
  if(!sendingInProgress) {
    logger->info(MSG_SEND_PEER_MESSAGE,
		 cuid, peer->ipaddr.c_str(), peer->port,
		 toString().c_str());
    getMessageHeader();
    peerConnection->sendMessage(msgHeader, getMessageHeaderLength());
    off_t pieceDataOffset =
      (off_t)index*_downloadContext->getPieceLength()+begin;
    size_t writtenLength = sendPieceData(pieceDataOffset, blockLength);
    logger->debug("msglength = %lu bytes",
		  static_cast<unsigned long>(getMessageHeaderLength()+
					     blockLength));
    peer->updateUploadLength(writtenLength);
  } else {
    ssize_t writtenLength = peerConnection->sendPendingData();
    peer->updateUploadLength(writtenLength);
  }
  sendingInProgress = !peerConnection->sendBufferIsEmpty();
}

size_t BtPieceMessage::sendPieceData(off_t offset, size_t length) const {
  assert(length <= 16*1024);
  unsigned char buf[16*1024];
  if(pieceStorage->getDiskAdaptor()->readData(buf, length, offset) ==
     static_cast<ssize_t>(length)) {
    return peerConnection->sendMessage(buf, length);
  } else {
    throw DL_ABORT_EX(EX_DATA_READ);
  }
}

std::string BtPieceMessage::toString() const {
  return strconcat(NAME, " index=", Util::itos(index), ", begin=",
		   Util::itos(begin), ", length=", Util::itos(blockLength));
}

bool BtPieceMessage::checkPieceHash(const PieceHandle& piece) {
  if(piece->isHashCalculated()) {
    logger->debug("Hash is available!! index=%lu",
		  static_cast<unsigned long>(piece->getIndex()));
    return
      piece->getHashString()==_downloadContext->getPieceHash(piece->getIndex());
  } else {
    off_t offset = (off_t)piece->getIndex()*_downloadContext->getPieceLength();
    
    return MessageDigestHelper::staticSHA1Digest(pieceStorage->getDiskAdaptor(), offset, piece->getLength())
      == _downloadContext->getPieceHash(piece->getIndex());
  }
}

void BtPieceMessage::onNewPiece(const PieceHandle& piece) {
  logger->info(MSG_GOT_NEW_PIECE, cuid, piece->getIndex());
  pieceStorage->completePiece(piece);
  pieceStorage->advertisePiece(cuid, piece->getIndex());
}

void BtPieceMessage::onWrongPiece(const PieceHandle& piece) {
  logger->info(MSG_GOT_WRONG_PIECE, cuid, piece->getIndex());
  erasePieceOnDisk(piece);
  piece->clearAllBlock();
  piece->destroyHashContext();
  requestFactory->removeTargetPiece(piece);
}

void BtPieceMessage::erasePieceOnDisk(const PieceHandle& piece) {
  size_t BUFSIZE = 4096;
  unsigned char buf[BUFSIZE];
  memset(buf, 0, BUFSIZE);
  off_t offset = (off_t)piece->getIndex()*_downloadContext->getPieceLength();
  div_t res = div(piece->getLength(), BUFSIZE);
  for(int i = 0; i < res.quot; ++i) {
    pieceStorage->getDiskAdaptor()->writeData(buf, BUFSIZE, offset);
    offset += BUFSIZE;
  }
  if(res.rem > 0) {
    pieceStorage->getDiskAdaptor()->writeData(buf, res.rem, offset);
  }
}

void BtPieceMessage::onChokingEvent(const BtChokingEvent& event)
{
  if(!invalidate &&
     !sendingInProgress &&
     !peer->isInAmAllowedIndexSet(index)) {
    logger->debug(MSG_REJECT_PIECE_CHOKED,
		  cuid,
		  index,
		  begin,
		  blockLength);

    if(peer->isFastExtensionEnabled()) {
      BtMessageHandle rej = messageFactory->createRejectMessage(index,
								begin,
								blockLength);
      dispatcher->addMessageToQueue(rej);
    }
    invalidate = true;
  }
}

void BtPieceMessage::onCancelSendingPieceEvent
(const BtCancelSendingPieceEvent& event)
{
  if(!invalidate &&
     !sendingInProgress &&
     index == event.getIndex() &&
     begin == event.getBegin() &&
     blockLength == event.getLength()) {
    logger->debug(MSG_REJECT_PIECE_CANCEL,
		  cuid, index, begin, blockLength);
    if(peer->isFastExtensionEnabled()) {
      BtMessageHandle rej = messageFactory->createRejectMessage(index,
								begin,
								blockLength);
      dispatcher->addMessageToQueue(rej);
    }
    invalidate = true;
  } 
}

void BtPieceMessage::setDownloadContext
(const SharedHandle<DownloadContext>& downloadContext)
{
  _downloadContext = downloadContext;
}

} // namespace aria2
