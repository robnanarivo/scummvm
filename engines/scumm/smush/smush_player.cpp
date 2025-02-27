/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "common/config-manager.h"
#include "common/file.h"
#include "common/system.h"
#include "common/util.h"
#include "common/rect.h"

#include "audio/mixer.h"

#include "graphics/cursorman.h"
#include "graphics/palette.h"

#include "scumm/file.h"
#include "scumm/imuse_digi/dimuse_engine.h"
#include "scumm/scumm.h"
#include "scumm/scumm_v7.h"
#include "scumm/sound.h"
#include "scumm/smush/channel.h"
#include "scumm/smush/codec37.h"
#include "scumm/smush/codec47.h"
#include "scumm/smush/smush_font.h"
#include "scumm/smush/smush_mixer.h"
#include "scumm/smush/smush_player.h"

#include "scumm/insane/insane.h"

#include "audio/audiostream.h"
#include "audio/mixer.h"
#include "audio/decoders/mp3.h"
#include "audio/decoders/raw.h"
#include "audio/decoders/vorbis.h"

#include "common/zlib.h"

namespace Scumm {

static const int MAX_STRINGS = 200;
static const int ETRS_HEADER_LENGTH = 16;

class StringResource {
private:

	struct {
		int id;
		char *string;
	} _strings[MAX_STRINGS];

	int _nbStrings;
	int _lastId;
	const char *_lastString;

public:

	StringResource() :
		_nbStrings(0),
		_lastId(-1),
		_lastString(NULL) {
		for (int i = 0; i < MAX_STRINGS; i++) {
			_strings[i].id = 0;
			_strings[i].string = NULL;
		}
	}
	~StringResource() {
		for (int32 i = 0; i < _nbStrings; i++) {
			delete[] _strings[i].string;
		}
	}

	bool init(char *buffer, int32 length) {
		char *def_start = strchr(buffer, '#');
		while (def_start != NULL) {
			char *def_end = strchr(def_start, '\n');
			assert(def_end != NULL);

			char *id_end = def_end;
			while (id_end >= def_start && !Common::isDigit(*(id_end-1))) {
				id_end--;
			}

			assert(id_end > def_start);
			char *id_start = id_end;
			while (Common::isDigit(*(id_start - 1))) {
				id_start--;
			}

			char idstring[32];
			memcpy(idstring, id_start, id_end - id_start);
			idstring[id_end - id_start] = 0;
			int32 id = atoi(idstring);
			char *data_start = def_end;

			while (*data_start == '\n' || *data_start == '\r') {
				data_start++;
			}
			char *data_end = data_start;

			while (1) {
				if (data_end[-2] == '\r' && data_end[-1] == '\n' && data_end[0] == '\r' && data_end[1] == '\n') {
					break;
				}
				// In the Steam Mac version of The Dig, LF-LF is used
				// instead of CR-LF
				if (data_end[-2] == '\n' && data_end[-1] == '\n') {
					break;
				}
				// In Russian Full Throttle strings are finished with
				// just one pair of CR-LF
				if (data_end[-2] == '\r' && data_end[-1] == '\n' && data_end[0] == '#') {
					break;
				}
				data_end++;
				if (data_end >= buffer + length) {
					data_end = buffer + length;
					break;
				}
			}

			data_end -= 2;
			assert(data_end > data_start);
			char *value = new char[data_end - data_start + 1];
			assert(value);
			memcpy(value, data_start, data_end - data_start);
			value[data_end - data_start] = 0;
			char *line_start = value;
			char *line_end;

			while ((line_end = strchr(line_start, '\n'))) {
				line_start = line_end+1;
				if (line_start[0] == '/' && line_start[1] == '/') {
					line_start += 2;
					if	(line_end[-1] == '\r')
						line_end[-1] = ' ';
					else
						*line_end++ = ' ';
					memmove(line_end, line_start, strlen(line_start)+1);
				}
			}
			_strings[_nbStrings].id = id;
			_strings[_nbStrings].string = value;
			_nbStrings ++;
			def_start = strchr(data_end + 2, '#');
		}
		return true;
	}

	const char *get(int id) {
		if (id == _lastId) {
			return _lastString;
		}
		debugC(DEBUG_SMUSH, "StringResource::get(%d)", id);
		for (int i = 0; i < _nbStrings; i++) {
			if (_strings[i].id == id) {
				_lastId = id;
				_lastString = _strings[i].string;
				return _strings[i].string;
			}
		}
		warning("invalid string id : %d", id);
		_lastId = -1;
		_lastString = "unknown string";
		return _lastString;
	}
};

static StringResource *getStrings(ScummEngine *vm, const char *file, bool is_encoded) {
	debugC(DEBUG_SMUSH, "trying to read text resources from %s", file);
	ScummFile theFile;

	vm->openFile(theFile, file);
	if (!theFile.isOpen()) {
		return 0;
	}
	int32 length = theFile.size();
	char *filebuffer = new char [length + 1];
	assert(filebuffer);
	theFile.read(filebuffer, length);
	filebuffer[length] = 0;

	if (is_encoded && READ_BE_UINT32(filebuffer) == MKTAG('E','T','R','S')) {
		assert(length > ETRS_HEADER_LENGTH);
		length -= ETRS_HEADER_LENGTH;
		for (int i = 0; i < length; ++i) {
			filebuffer[i] = filebuffer[i + ETRS_HEADER_LENGTH] ^ 0xCC;
		}
		filebuffer[length] = '\0';
	}
	StringResource *sr = new StringResource;
	assert(sr);
	sr->init(filebuffer, length);
	delete[] filebuffer;
	return sr;
}

void SmushPlayer::timerCallback() {
	parseNextFrame();
}

SmushPlayer::SmushPlayer(ScummEngine_v7 *scumm, IMuseDigital *imuseDigital) {
	_vm = scumm;
	_imuseDigital = imuseDigital;
	_nbframes = 0;
	_codec37 = 0;
	_codec47 = 0;
	_smixer = NULL;
	_strings = NULL;
	_sf[0] = NULL;
	_sf[1] = NULL;
	_sf[2] = NULL;
	_sf[3] = NULL;
	_sf[4] = NULL;
	_base = NULL;
	_frameBuffer = NULL;
	_specialBuffer = NULL;

	_seekPos = -1;

	_skipNext = false;
	_dst = NULL;
	_storeFrame = false;
	_compressedFileMode = false;
	_width = 0;
	_height = 0;
	_IACTpos = 0;
	_speed = -1;
	_insanity = false;
	_middleAudio = false;
	_skipPalette = false;
	_IACTstream = NULL;
	_smixer = _vm->_smixer;
	_paused = false;
	_pauseStartTime = 0;
	_pauseTime = 0;

	for (int i = 0; i < 4; i++)
		_iactTable[i] = 0;

	_IACTchannel = new Audio::SoundHandle();
	_compressedFileSoundHandle = new Audio::SoundHandle();
}

SmushPlayer::~SmushPlayer() {
	delete _IACTchannel;
	delete _compressedFileSoundHandle;
}

void SmushPlayer::init(int32 speed) {
	VirtScreen *vs = &_vm->_virtscr[kMainVirtScreen];

	_frame = 0;
	_speed = speed;
	_endOfFile = false;

	_vm->_smushVideoShouldFinish = false;
	_vm->_smushActive = true;

	_vm->setDirtyColors(0, 255);
	_dst = vs->getPixels(0, 0);

	// HACK HACK HACK: This is an *evil* trick, beware!
	// We do this to fix bug #1792. A proper solution would change all the
	// drawing code to use the pitch value specified by the virtual screen.
	// However, since a lot of the SMUSH code currently assumes the screen
	// width and pitch to be equal, this will require lots of changes. So
	// we resort to this hackish solution for now.
	_origPitch = vs->pitch;
	_origNumStrips = _vm->_gdi->_numStrips;
	vs->pitch = vs->w;
	_vm->_gdi->_numStrips = vs->w / 8;

	_vm->_mixer->stopHandle(*_compressedFileSoundHandle);
	_vm->_mixer->stopHandle(*_IACTchannel);
	_IACTpos = 0;
	_vm->_smixer->stop();
}

void SmushPlayer::release() {
	_vm->_smushVideoShouldFinish = true;

	for (int i = 0; i < 5; i++) {
		delete _sf[i];
		_sf[i] = NULL;
	}

	delete _strings;
	_strings = NULL;

	delete _base;
	_base = NULL;

	free(_specialBuffer);
	_specialBuffer = NULL;

	free(_frameBuffer);
	_frameBuffer = NULL;

	_IACTstream = NULL;

	_vm->_smushActive = false;
	_vm->_fullRedraw = true;

	// HACK HACK HACK: This is an *evil* trick, beware! See above for
	// some explanation.
	_vm->_virtscr[kMainVirtScreen].pitch = _origPitch;
	_vm->_gdi->_numStrips = _origNumStrips;

	delete _codec37;
	_codec37 = 0;
	delete _codec47;
	_codec47 = 0;
}

void SmushPlayer::handleSoundBuffer(int32 track_id, int32 index, int32 max_frames, int32 flags, int32 vol, int32 pan, Common::SeekableReadStream &b, int32 size) {
	debugC(DEBUG_SMUSH, "SmushPlayer::handleSoundBuffer(%d, %d)", track_id, index);
//	if ((flags & 128) == 128) {
//		return;
//	}
//	if ((flags & 64) == 64) {
//		return;
//	}
	SmushChannel *c = _smixer->findChannel(track_id);
	if (c == NULL) {
		c = new SaudChannel(track_id);
		_smixer->addChannel(c);
	}

	if (_middleAudio || index == 0) {
		c->setParameters(max_frames, flags, vol, pan, index);
	} else {
		c->checkParameters(index, max_frames, flags, vol, pan);
	}
	_middleAudio = false;
	c->appendData(b, size);
}

void SmushPlayer::handleSoundFrame(int32 subSize, Common::SeekableReadStream &b) {
	debugC(DEBUG_SMUSH, "SmushPlayer::handleSoundFrame()");

	int32 track_id = b.readUint16LE();
	int32 index = b.readUint16LE();
	int32 max_frames = b.readUint16LE();
	int32 flags = b.readUint16LE();
	int32 vol = b.readByte();
	int32 pan = b.readSByte();
	if (index == 0) {
		debugC(DEBUG_SMUSH, "track_id:%d, max_frames:%d, flags:%d, vol:%d, pan:%d", track_id, max_frames, flags, vol, pan);
	}
	int32 size = subSize - 10;
	handleSoundBuffer(track_id, index, max_frames, flags, vol, pan, b, size);
}

void SmushPlayer::handleStore(int32 subSize, Common::SeekableReadStream &b) {
	debugC(DEBUG_SMUSH, "SmushPlayer::handleStore()");
	assert(subSize >= 4);
	_storeFrame = true;
}

void SmushPlayer::handleFetch(int32 subSize, Common::SeekableReadStream &b) {
	debugC(DEBUG_SMUSH, "SmushPlayer::handleFetch()");
	assert(subSize >= 6);

	if (_frameBuffer != NULL) {
		memcpy(_dst, _frameBuffer, _width * _height);
	}
}

void SmushPlayer::handleIACT(int32 subSize, Common::SeekableReadStream &b) {
	debugC(DEBUG_SMUSH, "SmushPlayer::IACT()");
	assert(subSize >= 8);

	int code = b.readUint16LE();
	int flags = b.readUint16LE();
	int unknown = b.readSint16LE();
	int userId = b.readUint16LE();

	if ((code != 8) && (flags != 46)) {
		_vm->_insane->procIACT(_dst, 0, 0, 0, b, 0, 0, code, flags, unknown, userId);
		return;
	}

	if (_compressedFileMode) {
		return;
	}

	assert(flags == 46 && unknown == 0);
	/*int track_id =*/ b.readUint16LE();
	int index = b.readUint16LE();
	int nbframes = b.readUint16LE();
	/*int32 size =*/ b.readUint32LE();
	int32 bsize = subSize - 18;

	if (_vm->_game.id == GID_CMI) {
		// TODO: Move this code into another SmushChannel subclass?
		byte *src = (byte *)malloc(bsize);
		b.read(src, bsize);
		byte *d_src = src;
		byte value;

		while (bsize > 0) {
			if (_IACTpos >= 2) {
				int32 len = READ_BE_UINT16(_IACToutput) + 2;
				len -= _IACTpos;
				if (len > bsize) {
					memcpy(_IACToutput + _IACTpos, d_src, bsize);
					_IACTpos += bsize;
					bsize = 0;
				} else {
					byte *output_data = (byte *)malloc(4096);

					memcpy(_IACToutput + _IACTpos, d_src, len);
					byte *dst = output_data;
					byte *d_src2 = _IACToutput;
					d_src2 += 2;
					int32 count = 1024;
					byte variable1 = *d_src2++;
					byte variable2 = variable1 / 16;
					variable1 &= 0x0f;
					do {
						value = *(d_src2++);
						if (value == 0x80) {
							*dst++ = *d_src2++;
							*dst++ = *d_src2++;
						} else {
							int16 val = (int8)value << variable2;
							*dst++ = val >> 8;
							*dst++ = (byte)(val);
						}
						value = *(d_src2++);
						if (value == 0x80) {
							*dst++ = *d_src2++;
							*dst++ = *d_src2++;
						} else {
							int16 val = (int8)value << variable1;
							*dst++ = val >> 8;
							*dst++ = (byte)(val);
						}
					} while (--count);

					if (!_IACTstream) {
						_IACTstream = Audio::makeQueuingAudioStream(22050, true);
						_vm->_mixer->playStream(Audio::Mixer::kSFXSoundType, _IACTchannel, _IACTstream);
					}
					_IACTstream->queueBuffer(output_data, 0x1000, DisposeAfterUse::YES, Audio::FLAG_STEREO | Audio::FLAG_16BITS);

					bsize -= len;
					d_src += len;
					_IACTpos = 0;
				}
			} else {
				if (bsize > 1 && _IACTpos == 0) {
					*(_IACToutput + 0) = *d_src++;
					_IACTpos = 1;
					bsize--;
				}
				*(_IACToutput + _IACTpos) = *d_src++;
				_IACTpos++;
				bsize--;
			}
		}

		free(src);
	} else if ((_vm->_game.id == GID_DIG) && !(_vm->_game.features & GF_DEMO)) {
		int bufId, volume, paused, curSoundId;

		byte *dataBuffer = (byte *)malloc(bsize);
		b.read(dataBuffer, bsize);

		switch (userId) {
		case 1:
			bufId = 1;
			volume = 127;
			break;
		case 2:
			bufId = 2;
			volume = 127;
			break;
		case 3:
			bufId = 3;
			volume = 127;
			break;
		default:
			if (userId >= 100 && userId <= 163) {
				bufId = DIMUSE_BUFFER_SPEECH;
				volume = 2 * userId - 200;
			} else if (userId >= 200 && userId <= 263) {
				bufId = DIMUSE_BUFFER_MUSIC;
				volume = 2 * userId - 400;
			} else if (userId >= 300 && userId <= 363) {
				bufId = DIMUSE_BUFFER_SMUSH;
				volume = 2 * userId - 600;
			} else {
				free(dataBuffer);
				error("SmushPlayer::handleIACT(): ERROR: got invalid userID (%d)", userId);
			}
			break;
		}

		paused = nbframes - index == 1;

		// Apparently this is expected to happen (e.g.: Brink's death video)
		if (index && _iactTable[bufId] - index != -1) {
			free(dataBuffer);
			debugC(DEBUG_SMUSH, "SmushPlayer::handleIACT(): WARNING: got out of order block");
			return;
		}

		_iactTable[bufId] = index;

		if (index) {
			if (_imuseDigital->diMUSEGetParam(bufId + DIMUSE_SMUSH_SOUNDID, DIMUSE_P_SND_TRACK_NUM)) {
				_imuseDigital->diMUSEFeedStream(bufId + DIMUSE_SMUSH_SOUNDID, dataBuffer, subSize - 18, paused);
				free(dataBuffer);
				return;
			}
			free(dataBuffer);
			error("SmushPlayer::handleIACT(): ERROR: got unexpected non-zero IACT block, bufID %d", bufId);
		} else {
			if (READ_BE_UINT32(dataBuffer) != MKTAG('i', 'M', 'U', 'S')) {
				free(dataBuffer);
				error("SmushPlayer::handleIACT(): ERROR: got non-IMUS IACT block");
			}

			curSoundId = 0;
			do {
				curSoundId = _imuseDigital->diMUSEGetNextSound(curSoundId);
				if (!curSoundId)
					break;
			} while (_imuseDigital->diMUSEGetParam(curSoundId, DIMUSE_P_SND_HAS_STREAM) != 1 || _imuseDigital->diMUSEGetParam(curSoundId, DIMUSE_P_STREAM_BUFID) != bufId);

			if (!curSoundId) {
				// There isn't any previous sound running: start a new stream
				if (_imuseDigital->diMUSEStartStream(bufId + DIMUSE_SMUSH_SOUNDID, 126, bufId)) {
					free(dataBuffer);
					error("SmushPlayer::handleIACT(): ERROR: couldn't start stream");
				}
			} else {
				// There's an old sound running: switch the stream from the old one to the new one
				_imuseDigital->diMUSESwitchStream(curSoundId, bufId + DIMUSE_SMUSH_SOUNDID, bufId == 2 ? 1000 : 150, 0, 0);
			}

			_imuseDigital->diMUSESetParam(bufId + DIMUSE_SMUSH_SOUNDID, DIMUSE_P_VOLUME, volume);

			if (bufId == DIMUSE_BUFFER_SPEECH) {
				_imuseDigital->diMUSESetParam(bufId + DIMUSE_SMUSH_SOUNDID, DIMUSE_P_GROUP, DIMUSE_GROUP_SPEECH);
			} else if (bufId == DIMUSE_BUFFER_MUSIC) {
				_imuseDigital->diMUSESetParam(bufId + DIMUSE_SMUSH_SOUNDID, DIMUSE_P_GROUP, DIMUSE_GROUP_MUSIC);
			} else {
				_imuseDigital->diMUSESetParam(bufId + DIMUSE_SMUSH_SOUNDID, DIMUSE_P_GROUP, DIMUSE_GROUP_SFX);
			}

			_imuseDigital->diMUSEFeedStream(bufId + DIMUSE_SMUSH_SOUNDID, dataBuffer, subSize - 18, paused);
			free(dataBuffer);
			return;
		}
	}
}

void SmushPlayer::handleTextResource(uint32 subType, int32 subSize, Common::SeekableReadStream &b) {
	int pos_x = b.readSint16LE();
	int pos_y = b.readSint16LE();
	int flags = b.readSint16LE();
	int left = b.readSint16LE();
	int top = b.readSint16LE();
	int width = b.readSint16LE();
	int height = b.readSint16LE();
	/*int32 unk2 =*/ b.readUint16LE();

	const char *str;
	char *string = NULL, *string2 = NULL;
	if (subType == MKTAG('T','E','X','T')) {
		string = (char *)malloc(subSize - 16);
		str = string;
		b.read(string, subSize - 16);
	} else {
		int string_id = b.readUint16LE();
		if (!_strings)
			return;
		str = _strings->get(string_id);
	}

	// if subtitles disabled and bit 3 is set, then do not draw
	//
	// Query ConfMan here. However it may be slower, but
	// player may want to switch the subtitles on or off during the
	// playback. This fixes bug #2812
	if ((!ConfMan.getBool("subtitles")) && ((flags & 8) == 8))
		return;

	bool isCJKComi = (_vm->_game.id == GID_CMI && _vm->_useCJKMode);
	int color = 15;
	int fontId = isCJKComi ? 1 : 0;

	while (*str == '/') {
		str++; // For Full Throttle text resources
	}

	byte transBuf[512];
	if (_vm->_game.id == GID_CMI) {
		_vm->translateText((const byte *)str - 1, transBuf);
		while (*str++ != '/')
			;
		string2 = (char *)transBuf;

		// If string2 contains formatting information there probably
		// wasn't any translation for it in the language.tab file. In
		// that case, pretend there is no string2.
		if (string2[0] == '^')
			string2[0] = 0;
	}

	while (str[0] == '^') {
		switch (str[1]) {
		case 'f':
			fontId = str[3] - '0';
			str += 4;
			break;
		case 'c':
			color = str[4] - '0' + 10 *(str[3] - '0');
			str += 5;
		break;
		default:
			error("invalid escape code in text string");
		}
	}

	if (_vm->_game.id == GID_CMI && string2[0] != 0)
		str = string2;

	// This is a hack from the original COMI CJK interpreter. Its purpose is to avoid
	// ugly combinations of two byte characters (rendered with the respective special
	// font) and standard one byte (NUT font) characters (see bug #11947).
	if (isCJKComi && !(fontId == 0 && color == 1)) {
		fontId = 1;
		color = 255;
	}

	SmushFont *sf = getFont(fontId);
	assert(sf != NULL);

	// The hack that used to be here to prevent bug #2220 is no longer necessary and
	// has been removed. The font renderer can handle all ^codes it encounters (font
	// changes on the fly will be ignored for Smush texts, since our code design does
	// not permit it and the feature isn't used anyway).

	if (_vm->_language == Common::HE_ISR && !(flags & kStyleAlignCenter)) {
		flags |= kStyleAlignRight;
		pos_x = _width - 1 - pos_x;
	}

	TextStyleFlags flg = (TextStyleFlags)(flags & 7);
	// flags:
	// bit 0 - center                  0x01
	// bit 1 - not used (align right)  0x02
	// bit 2 - word wrap               0x04
	// bit 3 - switchable              0x08
	// bit 4 - fill background         0x10
	// bit 5 - outline/shadow          0x20        (apparently only set by the text renderer itself, not from the smush data)
	// bit 6 - vertical fix (COMI)     0x40        (COMI handles this in the printing method, but I haven't seen a case where it is used)
	// bit 7 - skip ^ codes (COMI)     0x80        (should be irrelevant for Smush, we strip these commands anyway)
	// bit 8 - no vertical fix (COMI)  0x100       (COMI handles this in the printing method, but I haven't seen a case where it is used)

	if (flg & kStyleWordWrap) {
		// COMI has to do it all a bit different, of course. SCUMM7 games immediately render the text from here and actually use the clipping data
		// provided by the text resource. COMI does not render directly, but enqueues a blast string (which is then drawn through the usual main
		// loop routines). During that process the rect data will get dumped and replaced with the following default values. It's hard to tell
		// whether this is on purpose or not (the text looks not necessarily better or worse, just different), so we follow the original...
		if (_vm->_game.id == GID_CMI) {
			left = top = 10;
			width = _width - 20;
			height = _height - 20;
		}
		Common::Rect clipRect(MAX<int>(0, left), MAX<int>(0, top), MIN<int>(left + width, _width), MIN<int>(top + height, _height));
		sf->drawStringWrap(str, _dst, clipRect, pos_x, pos_y, color, flg);
	} else {
		// Similiar to the wrapped text, COMI will pass on rect coords here, which will later be lost. Unlike with the wrapped text, it will
		// finally use the full screen dimenstions. SCUMM7 renders directly from here (see comment above), but also with the full screen.
		Common::Rect clipRect(0, 0, _width, _height);
		sf->drawString(str, _dst, clipRect, pos_x, pos_y, color, flg);
	}

	free(string);
}

const char *SmushPlayer::getString(int id) {
	return _strings->get(id);
}

bool SmushPlayer::readString(const char *file) {
	const char *i = strrchr(file, '.');
	if (i == NULL) {
		error("invalid filename : %s", file);
	}
	char fname[260];
	memcpy(fname, file, i - file);
	strcpy(fname + (i - file), ".trs");
	if ((_strings = getStrings(_vm, fname, false)) != 0) {
		return true;
	}

	if (_vm->_game.id == GID_DIG && (_strings = getStrings(_vm, "digtxt.trs", true)) != 0) {
		return true;
	}
	return false;
}

void SmushPlayer::readPalette(byte *out, Common::SeekableReadStream &in) {
	in.read(out, 0x300);
}

static byte delta_color(byte org_color, int16 delta_color) {
	int t = (org_color * 129 + delta_color) / 128;
	return CLIP(t, 0, 255);
}

void SmushPlayer::handleDeltaPalette(int32 subSize, Common::SeekableReadStream &b) {
	debugC(DEBUG_SMUSH, "SmushPlayer::handleDeltaPalette()");

	if (subSize == 0x300 * 3 + 4) {

		b.readUint16LE();
		b.readUint16LE();

		for (int i = 0; i < 0x300; i++) {
			_deltaPal[i] = b.readUint16LE();
		}
		readPalette(_pal, b);
		setDirtyColors(0, 255);
	} else if (subSize == 6) {

		b.readUint16LE();
		b.readUint16LE();
		b.readUint16LE();

		for (int i = 0; i < 0x300; i++) {
			_pal[i] = delta_color(_pal[i], _deltaPal[i]);
		}
		setDirtyColors(0, 255);
	} else {
		error("SmushPlayer::handleDeltaPalette() Wrong size for DeltaPalette");
	}
}

void SmushPlayer::handleNewPalette(int32 subSize, Common::SeekableReadStream &b) {
	debugC(DEBUG_SMUSH, "SmushPlayer::handleNewPalette()");
	assert(subSize >= 0x300);

	if (_skipPalette)
		return;

	readPalette(_pal, b);
	setDirtyColors(0, 255);
}

void smush_decode_codec1(byte *dst, const byte *src, int left, int top, int width, int height, int pitch);
void smush_decode_codec20(byte *dst, const byte *src, int left, int top, int width, int height, int pitch);

void SmushPlayer::decodeFrameObject(int codec, const uint8 *src, int left, int top, int width, int height) {
	if ((height == 242) && (width == 384)) {
		if (_specialBuffer == 0)
			_specialBuffer = (byte *)malloc(242 * 384);
		_dst = _specialBuffer;
	} else if ((height > _vm->_screenHeight) || (width > _vm->_screenWidth))
		return;
	// FT Insane uses smaller frames to draw overlays with moving objects
	// Other .san files do have them as well but their purpose in unknown
	// and often it causes memory overdraw. So just skip those frames
	else if (!_insanity && ((height != _vm->_screenHeight) || (width != _vm->_screenWidth)))
		return;

	if ((height == 242) && (width == 384)) {
		_width = width;
		_height = height;
	} else {
		_width = _vm->_screenWidth;
		_height = _vm->_screenHeight;
	}

	switch (codec) {
	case 1:
	case 3:
		smush_decode_codec1(_dst, src, left, top, width, height, _vm->_screenWidth);
		break;
	case 37:
		if (!_codec37)
			_codec37 = new Codec37Decoder(width, height);
		if (_codec37)
			_codec37->decode(_dst, src);
		break;
	case 47:
		if (!_codec47)
			_codec47 = new Codec47Decoder(width, height);
		if (_codec47)
			_codec47->decode(_dst, src);
		break;
	case 20:
		// Used by Full Throttle Classic (from Remastered)
		smush_decode_codec20(_dst, src, left, top, width, height, _vm->_screenWidth);
		break;
	default:
		error("Invalid codec for frame object : %d", codec);
	}

	if (_storeFrame) {
		if (_frameBuffer == NULL) {
			_frameBuffer = (byte *)malloc(_width * _height);
		}
		memcpy(_frameBuffer, _dst, _width * _height);
		_storeFrame = false;
	}
}

#ifdef USE_ZLIB
void SmushPlayer::handleZlibFrameObject(int32 subSize, Common::SeekableReadStream &b) {
	if (_skipNext) {
		_skipNext = false;
		return;
	}

	int32 chunkSize = subSize;
	byte *chunkBuffer = (byte *)malloc(chunkSize);
	assert(chunkBuffer);
	b.read(chunkBuffer, chunkSize);

	unsigned long decompressedSize = READ_BE_UINT32(chunkBuffer);
	byte *fobjBuffer = (byte *)malloc(decompressedSize);
	if (!Common::uncompress(fobjBuffer, &decompressedSize, chunkBuffer + 4, chunkSize - 4))
		error("SmushPlayer::handleZlibFrameObject() Zlib uncompress error");
	free(chunkBuffer);

	byte *ptr = fobjBuffer;
	int codec = READ_LE_UINT16(ptr); ptr += 2;
	int left = READ_LE_UINT16(ptr); ptr += 2;
	int top = READ_LE_UINT16(ptr); ptr += 2;
	int width = READ_LE_UINT16(ptr); ptr += 2;
	int height = READ_LE_UINT16(ptr); ptr += 2;

	decodeFrameObject(codec, fobjBuffer + 14, left, top, width, height);

	free(fobjBuffer);
}
#endif

void SmushPlayer::handleFrameObject(int32 subSize, Common::SeekableReadStream &b) {
	assert(subSize >= 14);
	if (_skipNext) {
		_skipNext = false;
		return;
	}

	int codec = b.readUint16LE();
	int left = b.readUint16LE();
	int top = b.readUint16LE();
	int width = b.readUint16LE();
	int height = b.readUint16LE();

	b.readUint16LE();
	b.readUint16LE();

	int32 chunk_size = subSize - 14;
	byte *chunk_buffer = (byte *)malloc(chunk_size);
	assert(chunk_buffer);
	b.read(chunk_buffer, chunk_size);

	decodeFrameObject(codec, chunk_buffer, left, top, width, height);

	free(chunk_buffer);
}

void SmushPlayer::handleFrame(int32 frameSize, Common::SeekableReadStream &b) {
	debugC(DEBUG_SMUSH, "SmushPlayer::handleFrame(%d)", _frame);
	_skipNext = false;

	if (_insanity) {
		_vm->_insane->procPreRendering();
	}

	while (frameSize > 0) {
		const uint32 subType = b.readUint32BE();
		const int32 subSize = b.readUint32BE();
		const int32 subOffset = b.pos();
		switch (subType) {
		case MKTAG('N','P','A','L'):
			handleNewPalette(subSize, b);
			break;
		case MKTAG('F','O','B','J'):
			handleFrameObject(subSize, b);
			break;
#ifdef USE_ZLIB
		case MKTAG('Z','F','O','B'):
			handleZlibFrameObject(subSize, b);
			break;
#endif
		case MKTAG('P','S','A','D'):
			if (!_compressedFileMode)
				handleSoundFrame(subSize, b);
			break;
		case MKTAG('T','R','E','S'):
			handleTextResource(subType, subSize, b);
			break;
		case MKTAG('X','P','A','L'):
			handleDeltaPalette(subSize, b);
			break;
		case MKTAG('I','A','C','T'):
			handleIACT(subSize, b);
			break;
		case MKTAG('S','T','O','R'):
			handleStore(subSize, b);
			break;
		case MKTAG('F','T','C','H'):
			handleFetch(subSize, b);
			break;
		case MKTAG('S','K','I','P'):
			_vm->_insane->procSKIP(subSize, b);
			break;
		case MKTAG('T','E','X','T'):
			handleTextResource(subType, subSize, b);
			break;
		default:
			error("Unknown frame subChunk found : %s, %d", tag2str(subType), subSize);
		}

		frameSize -= subSize + 8;
		b.seek(subOffset + subSize, SEEK_SET);
		if (subSize & 1) {
			b.skip(1);
			frameSize--;
		}
	}

	if (_insanity) {
		_vm->_insane->procPostRendering(_dst, 0, 0, 0, _frame, _nbframes-1);
	}

	if (_width != 0 && _height != 0) {
		updateScreen();
	}
	_smixer->handleFrame();

	_frame++;
}

void SmushPlayer::handleAnimHeader(int32 subSize, Common::SeekableReadStream &b) {
	debugC(DEBUG_SMUSH, "SmushPlayer::handleAnimHeader()");
	assert(subSize >= 0x300 + 6);

	/* _version = */ b.readUint16LE();
	_nbframes = b.readUint16LE();
	b.readUint16LE();

	if (_skipPalette)
		return;

	readPalette(_pal, b);
	setDirtyColors(0, 255);
}

void SmushPlayer::setupAnim(const char *file) {
	if (_insanity) {
		if (!((_vm->_game.features & GF_DEMO) && (_vm->_game.platform == Common::kPlatformDOS)))
			readString("mineroad.trs");
	} else
		readString(file);
}

SmushFont *SmushPlayer::getFont(int font) {
	char file_font[11];

	if (_sf[font])
		return _sf[font];

	if (_vm->_game.id == GID_FT) {
		if (!((_vm->_game.features & GF_DEMO) && (_vm->_game.platform == Common::kPlatformDOS))) {
			const char *ft_fonts[] = {
				"scummfnt.nut",
				"techfnt.nut",
				"titlfnt.nut",
				"specfnt.nut"
			};

			assert(font >= 0 && font < ARRAYSIZE(ft_fonts));

			_sf[font] = new SmushFont(_vm, ft_fonts[font], true);
		}
	} else {
		int numFonts = (_vm->_game.id == GID_CMI && !(_vm->_game.features & GF_DEMO)) ? 5 : 4;
		assert(font >= 0 && font < numFonts);
		sprintf(file_font, "font%d.nut", font);
		_sf[font] = new SmushFont(_vm, file_font, _vm->_game.id == GID_DIG && font != 0);
	}

	assert(_sf[font]);
	return _sf[font];
}

void SmushPlayer::parseNextFrame() {

	if (_seekPos >= 0) {
		if (_smixer)
			_smixer->stop();

		if (_seekFile.size() > 0) {
			delete _base;

			ScummFile *tmp = new ScummFile();
			if (!g_scumm->openFile(*tmp, _seekFile))
				error("SmushPlayer: Unable to open file %s", _seekFile.c_str());
			_base = tmp;
			_base->readUint32BE();
			_baseSize = _base->readUint32BE();

			if (_seekPos > 0) {
				assert(_seekPos > 8);
				// In this case we need to get palette and number of frames
				const uint32 subType = _base->readUint32BE();
				const int32 subSize = _base->readUint32BE();
				const int32 subOffset = _base->pos();
				assert(subType == MKTAG('A','H','D','R'));
				handleAnimHeader(subSize, *_base);
				_base->seek(subOffset + subSize, SEEK_SET);

				_middleAudio = true;
				_seekPos -= 8;
			} else {
				// We need this in Full Throttle when entering/leaving
				// the old mine road.
				tryCmpFile(_seekFile.c_str());
			}
			_skipPalette = false;
		} else {
			_skipPalette = true;
		}

		_base->seek(_seekPos + 8, SEEK_SET);
		_frame = _seekFrame;
		_startFrame = _frame;
		_startTime = _vm->_system->getMillis();

		_seekPos = -1;
	}

	assert(_base);

	const uint32 subType = _base->readUint32BE();
	const int32 subSize = _base->readUint32BE();
	const int32 subOffset = _base->pos();

	if (_base->pos() >= (int32)_baseSize) {
		_vm->_smushVideoShouldFinish = true;
		_endOfFile = true;
		return;
	}

	debug(3, "Chunk: %s at %x", tag2str(subType), subOffset);

	switch (subType) {
	case MKTAG('A','H','D','R'): // FT INSANE may seek file to the beginning
		handleAnimHeader(subSize, *_base);
		break;
	case MKTAG('F','R','M','E'):
		handleFrame(subSize, *_base);
		break;
	default:
		error("Unknown Chunk found at %x: %s, %d", subOffset, tag2str(subType), subSize);
	}

	_base->seek(subOffset + subSize, SEEK_SET);

	if (_insanity)
		_vm->_sound->processSound();

	_vm->_imuseDigital->flushTracks();
}

void SmushPlayer::setPalette(const byte *palette) {
	memcpy(_pal, palette, 0x300);
	setDirtyColors(0, 255);
}

void SmushPlayer::setPaletteValue(int n, byte r, byte g, byte b) {
	_pal[n * 3 + 0] = r;
	_pal[n * 3 + 1] = g;
	_pal[n * 3 + 2] = b;
	setDirtyColors(n, n);
}

void SmushPlayer::setDirtyColors(int min, int max) {
	if (_palDirtyMin > min)
		_palDirtyMin = min;
	if (_palDirtyMax < max)
		_palDirtyMax = max;
}

void SmushPlayer::warpMouse(int x, int y, int buttons) {
	_warpNeeded = true;
	_warpX = x;
	_warpY = y;
	_warpButtons = buttons;
}

void SmushPlayer::updateScreen() {
	uint32 end_time, start_time = _vm->_system->getMillis();
	_updateNeeded = true;
	end_time = _vm->_system->getMillis();
	debugC(DEBUG_SMUSH, "Smush stats: updateScreen( %03d )", end_time - start_time);
}

void SmushPlayer::insanity(bool flag) {
	_insanity = flag;
}

void SmushPlayer::seekSan(const char *file, int32 pos, int32 contFrame) {
	_seekFile = file ? file : "";
	_seekPos = pos;
	_seekFrame = contFrame;
	_pauseTime = 0;
}

void SmushPlayer::tryCmpFile(const char *filename) {
	_vm->_mixer->stopHandle(*_compressedFileSoundHandle);

	_compressedFileMode = false;
	const char *i = strrchr(filename, '.');
	if (i == NULL) {
		error("invalid filename : %s", filename);
	}
#if defined(USE_MAD) || defined(USE_VORBIS)
	char fname[260];
#endif
	Common::File *file = new Common::File();

	// FIXME: How about using AudioStream::openStreamFile instead of the code below?

#ifdef USE_VORBIS
	memcpy(fname, filename, i - filename);
	strcpy(fname + (i - filename), ".ogg");
	if (file->open(fname)) {
		_compressedFileMode = true;
		_vm->_mixer->playStream(Audio::Mixer::kSFXSoundType, _compressedFileSoundHandle, Audio::makeVorbisStream(file, DisposeAfterUse::YES));
		return;
	}
#endif
#ifdef USE_MAD
	memcpy(fname, filename, i - filename);
	strcpy(fname + (i - filename), ".mp3");
	if (file->open(fname)) {
		_compressedFileMode = true;
		_vm->_mixer->playStream(Audio::Mixer::kSFXSoundType, _compressedFileSoundHandle, Audio::makeMP3Stream(file, DisposeAfterUse::YES));
		return;
	}
#endif
	delete file;
}

void SmushPlayer::pause() {
	if (!_paused) {
		_paused = true;
		_pauseStartTime = _vm->_system->getMillis();
	}
}

void SmushPlayer::unpause() {
	if (_paused) {
		_paused = false;
		_pauseTime += (_vm->_system->getMillis() - _pauseStartTime);
		_pauseStartTime = 0;
	}
}

void SmushPlayer::play(const char *filename, int32 speed, int32 offset, int32 startFrame) {
	// Verify the specified file exists
	ScummFile f;
	_vm->openFile(f, filename);
	if (!f.isOpen()) {
		warning("SmushPlayer::play() File not found %s", filename);
		return;
	}
	f.close();

	_updateNeeded = false;
	_warpNeeded = false;
	_palDirtyMin = 256;
	_palDirtyMax = -1;

	// Hide mouse
	bool oldMouseState = CursorMan.showMouse(false);

	// Load the video
	_seekFile = filename;
	_seekPos = offset;
	_seekFrame = startFrame;
	_base = 0;

	setupAnim(filename);
	init(speed);

	_startTime = _vm->_system->getMillis();
	_startFrame = startFrame;
	_frame = startFrame;

	_pauseTime = 0;

	int skipped = 0;

	for (;;) {
		uint32 now, elapsed;
		bool skipFrame = false;

		if (_insanity) {
			// Seeking makes a mess of trying to sync the audio to
			// the sound. Synt to time instead.
			now = _vm->_system->getMillis() - _pauseTime;
			elapsed = now - _startTime;
		} else if (_vm->_mixer->isSoundHandleActive(*_compressedFileSoundHandle)) {
			// Compressed SMUSH files.
			elapsed = _vm->_mixer->getSoundElapsedTime(*_compressedFileSoundHandle);
		} else if (_vm->_mixer->isSoundHandleActive(*_IACTchannel)) {
			// Curse of Monkey Island SMUSH files.
			elapsed = _vm->_mixer->getSoundElapsedTime(*_IACTchannel);
		} else {
			// For other SMUSH files, we don't necessarily have any
			// one channel to sync against, so we have to use
			// elapsed real time.
			now = _vm->_system->getMillis() - _pauseTime;
			elapsed = now - _startTime;
		}

		if (elapsed >= ((_frame - _startFrame) * 1000) / _speed) {
			if (elapsed >= ((_frame + 1) * 1000) / _speed)
				skipFrame = true;
			else
				skipFrame = false;
			timerCallback();
		}

		_vm->scummLoop_handleSound();

		if (_warpNeeded) {
			_vm->_system->warpMouse(_warpX, _warpY);
			_warpNeeded = false;
		}
		_vm->parseEvents();
		_vm->processInput();
		if (_palDirtyMax >= _palDirtyMin) {
			_vm->_system->getPaletteManager()->setPalette(_pal + _palDirtyMin * 3, _palDirtyMin, _palDirtyMax - _palDirtyMin + 1);

			_palDirtyMax = -1;
			_palDirtyMin = 256;
			skipFrame = false;
		}
		if (skipFrame) {
			if (++skipped > 10) {
				skipFrame = false;
				skipped = 0;
			}
		} else
			skipped = 0;
		if (_updateNeeded) {
			if (!skipFrame) {
				// Workaround for bug #2415: "FT DEMO: assertion triggered
				// when playing movie". Some frames there are 384 x 224
				int w = MIN(_width, _vm->_screenWidth);
				int h = MIN(_height, _vm->_screenHeight);

				_vm->_system->copyRectToScreen(_dst, _width, 0, 0, w, h);
				_vm->_system->updateScreen();
				_updateNeeded = false;
			}
		}
		if (_endOfFile)
			break;
		if (_vm->shouldQuit() || _vm->_saveLoadFlag || _vm->_smushVideoShouldFinish) {
			_smixer->stop();
			_vm->_mixer->stopHandle(*_compressedFileSoundHandle);
			_vm->_mixer->stopHandle(*_IACTchannel);
			_IACTpos = 0;
			_imuseDigital->stopSMUSHAudio();
			break;
		}
		_vm->_system->delayMillis(10);
	}

	release();

	// Reset mouse state
	CursorMan.showMouse(oldMouseState);
}

} // End of namespace Scumm
