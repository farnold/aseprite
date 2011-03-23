/* ASE - Allegro Sprite Editor
 * Copyright (C) 2001-2011  David Capello
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"

#include "undo_transaction.h"

#include "base/unique_ptr.h"
#include "document_wrappers.h"
#include "gui/list.h"
#include "raster/blend.h"
#include "raster/cel.h"
#include "raster/dirty.h"
#include "raster/image.h"
#include "raster/layer.h"
#include "raster/mask.h"
#include "raster/palette.h"
#include "raster/quantization.h"
#include "raster/sprite.h"
#include "raster/stock.h"
#include "raster/undo_history.h"

UndoTransaction::UndoTransaction(DocumentWriter& document, const char* label)
{
  ASSERT(label != NULL);

  m_document = document;
  m_sprite = document->getSprite();
  m_undoHistory = document->getUndoHistory();
  m_committed = false;
  m_enabledFlag = m_undoHistory->isEnabled();

  if (isEnabled()) {
    m_undoHistory->setLabel(label);
    m_undoHistory->undo_open();
  }
}

UndoTransaction::~UndoTransaction()
{
  if (isEnabled()) {
    // Close the undo information.
    m_undoHistory->undo_close();

    // If it isn't committed, we have to rollback all changes.
    if (!m_committed) {
      // Undo the group of operations.
      m_undoHistory->doUndo();

      // Clear the redo (sorry to the user, here we lost the old redo
      // information).
      m_undoHistory->clearRedo();
    }
  }
}

void UndoTransaction::commit()
{
  m_committed = true;
}

void UndoTransaction::setNumberOfFrames(int frames)
{
  ASSERT(frames >= 1);

  // Save in undo the current totalFrames property
  if (isEnabled())
    m_undoHistory->undo_set_frames(m_sprite);

  // Change the property
  m_sprite->setTotalFrames(frames);
}

void UndoTransaction::setCurrentFrame(int frame)
{
  ASSERT(frame >= 0);

  if (isEnabled())
    m_undoHistory->undo_set_frame(m_sprite);

  m_sprite->setCurrentFrame(frame);
}

/**
 * Sets the current selected layer in the sprite.
 *
 * @param layer
 *   The layer to select. Can be NULL.
 */
void UndoTransaction::setCurrentLayer(Layer* layer)
{
  if (isEnabled())
    m_undoHistory->undo_set_layer(m_sprite);

  m_sprite->setCurrentLayer(layer);
}

void UndoTransaction::setSpriteSize(int w, int h)
{
  ASSERT(w > 0);
  ASSERT(h > 0);

  if (isEnabled())
    m_undoHistory->undo_set_size(m_sprite);

  m_sprite->setSize(w, h);
}

void UndoTransaction::cropSprite(int x, int y, int w, int h, int bgcolor)
{
  setSpriteSize(w, h);

  displaceLayers(m_sprite->getFolder(), -x, -y);

  Layer *background_layer = m_sprite->getBackgroundLayer();
  if (background_layer)
    cropLayer(background_layer, 0, 0, m_sprite->getWidth(), m_sprite->getHeight(), bgcolor);

  if (!m_sprite->getMask()->is_empty())
    setMaskPosition(m_sprite->getMask()->x-x, m_sprite->getMask()->y-y);
}

void UndoTransaction::autocropSprite(int bgcolor)
{
  int old_frame = m_sprite->getCurrentFrame();
  int x1, y1, x2, y2;
  int u1, v1, u2, v2;

  x1 = y1 = INT_MAX;
  x2 = y2 = INT_MIN;

  Image* image = image_new(m_sprite->getImgType(),
			   m_sprite->getWidth(),
			   m_sprite->getHeight());

  for (int frame=0; frame<m_sprite->getTotalFrames(); ++frame) {
    m_sprite->setCurrentFrame(frame);

    image_clear(image, 0);
    m_sprite->render(image, 0, 0);

    // TODO configurable (what color pixel to use as "refpixel",
    // here we are using the top-left pixel by default)
    if (image_shrink_rect(image, &u1, &v1, &u2, &v2,
			  image_getpixel(image, 0, 0))) {
      x1 = MIN(x1, u1);
      y1 = MIN(y1, v1);
      x2 = MAX(x2, u2);
      y2 = MAX(y2, v2);
    }
  }
  m_sprite->setCurrentFrame(old_frame);

  image_free(image);

  // do nothing
  if (x1 > x2 || y1 > y2)
    return;

  cropSprite(x1, y1, x2-x1+1, y2-y1+1, bgcolor);
}

void UndoTransaction::setImgType(int new_imgtype, DitheringMethod dithering_method)
{
  Image *old_image;
  Image *new_image;
  int c;

  if (m_sprite->getImgType() == new_imgtype)
    return;

  /* change imgtype of the stock of images */
  if (isEnabled())
    m_undoHistory->undo_int(m_sprite->getStock(), &m_sprite->getStock()->m_imgtype);

  m_sprite->getStock()->setImgType(new_imgtype);

  // Use the rgbmap for the specified sprite
  const RgbMap* rgbmap = m_sprite->getRgbMap();

  for (c=0; c<m_sprite->getStock()->size(); c++) {
    old_image = m_sprite->getStock()->getImage(c);
    if (!old_image)
      continue;

    new_image = quantization::convert_imgtype(old_image, new_imgtype, dithering_method, rgbmap,
					      // TODO check this out
					      m_sprite->getCurrentPalette(),
					      m_sprite->getBackgroundLayer() != NULL);

    this->replaceStockImage(c, new_image);
  }

  // Change sprite's "imgtype" field.
  if (isEnabled())
    m_undoHistory->undo_set_imgtype(m_sprite);

  m_sprite->setImgType(new_imgtype);

  // Regenerate extras
  m_document->destroyExtraCel();

  // change "sprite.palette"
  if (new_imgtype == IMAGE_GRAYSCALE) {
    if (isEnabled()) {
      // Save all palettes
      PalettesList palettes = m_sprite->getPalettes();
      for (PalettesList::iterator it = palettes.begin(); it != palettes.end(); ++it) {
	Palette* palette = *it;
	m_undoHistory->undo_remove_palette(m_sprite, palette);
      }
    }

    UniquePtr<Palette> graypal(Palette::createGrayscale());

    m_sprite->resetPalettes();
    m_sprite->setPalette(graypal, true);
  }
}

/**
 * Adds a new image in the stock.
 *
 * @return
 *   The image index in the stock.
 */
int UndoTransaction::addImageInStock(Image* image)
{
  ASSERT(image);

  // add the image in the stock
  int image_index = m_sprite->getStock()->addImage(image);

  if (isEnabled())
    m_undoHistory->undo_add_image(m_sprite->getStock(), image_index);

  return image_index;
}

/**
 * Removes and destroys the specified image in the stock.
 */
void UndoTransaction::removeImageFromStock(int image_index)
{
  ASSERT(image_index >= 0);

  Image* image = m_sprite->getStock()->getImage(image_index);
  ASSERT(image);

  if (isEnabled())
    m_undoHistory->undo_remove_image(m_sprite->getStock(), image_index);

  m_sprite->getStock()->removeImage(image);
  image_free(image);
}

void UndoTransaction::replaceStockImage(int image_index, Image* new_image)
{
  // get the current image in the 'image_index' position
  Image* old_image = m_sprite->getStock()->getImage(image_index);
  ASSERT(old_image);

  // replace the image in the stock
  if (isEnabled())
    m_undoHistory->undo_replace_image(m_sprite->getStock(), image_index);

  m_sprite->getStock()->replaceImage(image_index, new_image);

  // destroy the old image
  image_free(old_image);
}

/**
 * Creates a new transparent layer.
 */
Layer* UndoTransaction::newLayer()
{
  // new layer
  LayerImage* layer = new LayerImage(m_sprite);

  // add the layer in the sprite set
  if (isEnabled())
    m_undoHistory->undo_add_layer(m_sprite->getFolder(), layer);

  m_sprite->getFolder()->add_layer(layer);

  // select the new layer
  setCurrentLayer(layer);

  return layer;
}

/**
 * Removes and destroys the specified layer.
 */
void UndoTransaction::removeLayer(Layer* layer)
{
  ASSERT(layer);

  LayerFolder* parent = layer->get_parent();

  // if the layer to be removed is the selected layer
  if (layer == m_sprite->getCurrentLayer()) {
    Layer* layer_select = NULL;

    // select: previous layer, or next layer, or parent(if it is not the
    // main layer of sprite set)
    if (layer->get_prev())
      layer_select = layer->get_prev();
    else if (layer->get_next())
      layer_select = layer->get_next();
    else if (parent != m_sprite->getFolder())
      layer_select = parent;

    // select other layer
    setCurrentLayer(layer_select);
  }

  // remove the layer
  if (isEnabled())
    m_undoHistory->undo_remove_layer(layer);

  parent->remove_layer(layer);

  // destroy the layer
  delete layer;
}

void UndoTransaction::moveLayerAfter(Layer* layer, Layer* after_this)
{
  if (isEnabled())
    m_undoHistory->undo_move_layer(layer);

  layer->get_parent()->move_layer(layer, after_this);
}

void UndoTransaction::cropLayer(Layer* layer, int x, int y, int w, int h, int bgcolor)
{
  if (!layer->is_image())
    return;

  if (!layer->is_background())
    bgcolor = 0;

  CelIterator it = ((LayerImage*)layer)->getCelBegin();
  CelIterator end = ((LayerImage*)layer)->getCelEnd();
  for (; it != end; ++it)
    cropCel(*it, x, y, w, h, bgcolor);
}

/**
 * Moves every frame in @a layer with the offset (@a dx, @a dy).
 */
void UndoTransaction::displaceLayers(Layer* layer, int dx, int dy)
{
  switch (layer->getType()) {

    case GFXOBJ_LAYER_IMAGE: {
      CelIterator it = ((LayerImage*)layer)->getCelBegin();
      CelIterator end = ((LayerImage*)layer)->getCelEnd();
      for (; it != end; ++it) {
	Cel* cel = *it;
	setCelPosition(cel, cel->x+dx, cel->y+dy);
      }
      break;
    }

    case GFXOBJ_LAYER_FOLDER: {
      LayerIterator it = ((LayerFolder*)layer)->get_layer_begin();
      LayerIterator end = ((LayerFolder*)layer)->get_layer_end();
      for (; it != end; ++it)
	displaceLayers(*it, dx, dy);
      break;
    }

  }
}

void UndoTransaction::backgroundFromLayer(LayerImage* layer, int bgcolor)
{
  ASSERT(layer);
  ASSERT(layer->is_image());
  ASSERT(layer->is_readable());
  ASSERT(layer->is_writable());
  ASSERT(layer->getSprite() == m_sprite);
  ASSERT(m_sprite->getBackgroundLayer() == NULL);

  // create a temporary image to draw each frame of the new
  // `Background' layer
  UniquePtr<Image> bg_image_wrap(image_new(m_sprite->getImgType(),
					   m_sprite->getWidth(),
					   m_sprite->getHeight()));
  Image* bg_image = bg_image_wrap.get();

  CelIterator it = layer->getCelBegin();
  CelIterator end = layer->getCelEnd();

  for (; it != end; ++it) {
    Cel* cel = *it;
    ASSERT((cel->image > 0) &&
	   (cel->image < m_sprite->getStock()->size()));

    // get the image from the sprite's stock of images
    Image* cel_image = m_sprite->getStock()->getImage(cel->image);
    ASSERT(cel_image);

    image_clear(bg_image, bgcolor);
    image_merge(bg_image, cel_image,
		cel->x,
		cel->y,
		MID(0, cel->opacity, 255),
		layer->getBlendMode());

    // now we have to copy the new image (bg_image) to the cel...
    setCelPosition(cel, 0, 0);

    // same size of cel-image and bg-image
    if (bg_image->w == cel_image->w &&
	bg_image->h == cel_image->h) {
      if (isEnabled())
	m_undoHistory->undo_image(cel_image, 0, 0, cel_image->w, cel_image->h);

      image_copy(cel_image, bg_image, 0, 0);
    }
    else {
      replaceStockImage(cel->image, image_new_copy(bg_image));
    }
  }

  // Fill all empty cels with a flat-image filled with bgcolor
  for (int frame=0; frame<m_sprite->getTotalFrames(); frame++) {
    Cel* cel = layer->getCel(frame);
    if (!cel) {
      Image* cel_image = image_new(m_sprite->getImgType(), m_sprite->getWidth(), m_sprite->getHeight());
      image_clear(cel_image, bgcolor);

      // Add the new image in the stock
      int image_index = addImageInStock(cel_image);

      // Create the new cel and add it to the new background layer
      cel = cel_new(frame, image_index);
      addCel(layer, cel);
    }
  }

  configureLayerAsBackground(layer);
}

void UndoTransaction::layerFromBackground()
{
  ASSERT(m_sprite->getBackgroundLayer() != NULL);
  ASSERT(m_sprite->getCurrentLayer() != NULL);
  ASSERT(m_sprite->getCurrentLayer()->is_image());
  ASSERT(m_sprite->getCurrentLayer()->is_readable());
  ASSERT(m_sprite->getCurrentLayer()->is_writable());
  ASSERT(m_sprite->getCurrentLayer()->is_background());

  if (isEnabled()) {
    m_undoHistory->undo_data(
	      m_sprite->getCurrentLayer(),
	      m_sprite->getCurrentLayer()->flags_addr(),
	      sizeof(*m_sprite->getCurrentLayer()->flags_addr()));

    m_undoHistory->undo_set_layer_name(m_sprite->getCurrentLayer());
  }

  m_sprite->getCurrentLayer()->set_background(false);
  m_sprite->getCurrentLayer()->set_moveable(true);
  m_sprite->getCurrentLayer()->setName("Layer 0");
}

void UndoTransaction::flattenLayers(int bgcolor)
{
  Image* cel_image;
  Cel* cel;
  int frame;

  // create a temporary image
  UniquePtr<Image> image_wrap(image_new(m_sprite->getImgType(),
					m_sprite->getWidth(),
					m_sprite->getHeight()));
  Image* image = image_wrap.get();

  /* get the background layer from the sprite */
  LayerImage* background = m_sprite->getBackgroundLayer();
  if (!background) {
    /* if there aren't a background layer we must to create the background */
    background = new LayerImage(m_sprite);

    if (isEnabled())
      m_undoHistory->undo_add_layer(m_sprite->getFolder(), background);

    m_sprite->getFolder()->add_layer(background);

    if (isEnabled())
      m_undoHistory->undo_move_layer(background);
    
    background->configureAsBackground();
  }

  /* copy all frames to the background */
  for (frame=0; frame<m_sprite->getTotalFrames(); frame++) {
    /* clear the image and render this frame */
    image_clear(image, bgcolor);
    layer_render(m_sprite->getFolder(), image, 0, 0, frame);

    cel = background->getCel(frame);
    if (cel) {
      cel_image = m_sprite->getStock()->getImage(cel->image);
      ASSERT(cel_image != NULL);

      /* we have to save the current state of `cel_image' in the undo */
      if (isEnabled()) {
	Dirty* dirty = new Dirty(cel_image, image);
	dirty->saveImagePixels(cel_image);
	m_undoHistory->undo_dirty(cel_image, dirty);
	delete dirty;
      }
    }
    else {
      /* if there aren't a cel in this frame in the background, we
	 have to create a copy of the image for the new cel */
      cel_image = image_new_copy(image);
      /* TODO error handling: if (!cel_image) { ... } */

      /* here we create the new cel (with the new image `cel_image') */
      cel = cel_new(frame, m_sprite->getStock()->addImage(cel_image));
      /* TODO error handling: if (!cel) { ... } */

      /* and finally we add the cel in the background */
      background->addCel(cel);
    }

    image_copy(cel_image, image, 0, 0);
  }

  /* select the background */
  if (m_sprite->getCurrentLayer() != background) {
    if (isEnabled())
      m_undoHistory->undo_set_layer(m_sprite);

    m_sprite->setCurrentLayer(background);
  }

  /* remove old layers */
  LayerList layers = m_sprite->getFolder()->get_layers_list();
  LayerIterator it = layers.begin();
  LayerIterator end = layers.end();

  for (; it != end; ++it) {
    if (*it != background) {
      Layer* old_layer = *it;

      // remove the layer
      if (isEnabled())
	m_undoHistory->undo_remove_layer(old_layer);

      m_sprite->getFolder()->remove_layer(old_layer);

      // destroy the layer
      delete old_layer;
    }
  }
}

void UndoTransaction::configureLayerAsBackground(LayerImage* layer)
{
  if (isEnabled()) {
    m_undoHistory->undo_data(layer, layer->flags_addr(), sizeof(*layer->flags_addr()));
    m_undoHistory->undo_set_layer_name(layer);
    m_undoHistory->undo_move_layer(layer);
  }

  layer->configureAsBackground();
}

void UndoTransaction::newFrame()
{
  // add a new cel to every layer
  newFrameForLayer(m_sprite->getFolder(),
		   m_sprite->getCurrentFrame()+1);

  // increment frames counter in the sprite
  setNumberOfFrames(m_sprite->getTotalFrames()+1);

  // go to next frame (the new one)
  setCurrentFrame(m_sprite->getCurrentFrame()+1);
}

void UndoTransaction::newFrameForLayer(Layer* layer, int frame)
{
  ASSERT(layer);
  ASSERT(frame >= 0);

  switch (layer->getType()) {

    case GFXOBJ_LAYER_IMAGE:
      // displace all cels in '>=frame' to the next frame
      for (int c=m_sprite->getTotalFrames()-1; c>=frame; --c) {
	Cel* cel = static_cast<LayerImage*>(layer)->getCel(c);
	if (cel)
	  setCelFramePosition(cel, cel->frame+1);
      }

      copyPreviousFrame(layer, frame);
      break;

    case GFXOBJ_LAYER_FOLDER: {
      LayerIterator it = static_cast<LayerFolder*>(layer)->get_layer_begin();
      LayerIterator end = static_cast<LayerFolder*>(layer)->get_layer_end();

      for (; it != end; ++it)
	newFrameForLayer(*it, frame);
      break;
    }

  }
}

void UndoTransaction::removeFrame(int frame)
{
  ASSERT(frame >= 0);

  // Remove cels from this frame (and displace one position backward
  // all next frames)
  removeFrameOfLayer(m_sprite->getFolder(), frame);

  // New value for totalFrames propety
  int newTotalFrames = m_sprite->getTotalFrames()-1;

  // Move backward if we will be outside the range of frames
  if (m_sprite->getCurrentFrame() >= newTotalFrames)
    setCurrentFrame(newTotalFrames-1);

  // Decrement frames counter in the sprite
  setNumberOfFrames(newTotalFrames);
}

void UndoTransaction::removeFrameOfLayer(Layer* layer, int frame)
{
  ASSERT(layer);
  ASSERT(frame >= 0);

  switch (layer->getType()) {

    case GFXOBJ_LAYER_IMAGE:
      if (Cel* cel = static_cast<LayerImage*>(layer)->getCel(frame))
	removeCel(static_cast<LayerImage*>(layer), cel);

      for (++frame; frame<m_sprite->getTotalFrames(); ++frame)
	if (Cel* cel = static_cast<LayerImage*>(layer)->getCel(frame))
	  setCelFramePosition(cel, cel->frame-1);
      break;

    case GFXOBJ_LAYER_FOLDER: {
      LayerIterator it = static_cast<LayerFolder*>(layer)->get_layer_begin();
      LayerIterator end = static_cast<LayerFolder*>(layer)->get_layer_end();

      for (; it != end; ++it)
	removeFrameOfLayer(*it, frame);
      break;
    }

  }
}

/**
 * Copies the previous cel of @a frame to @frame.
 */
void UndoTransaction::copyPreviousFrame(Layer* layer, int frame)
{
  ASSERT(layer);
  ASSERT(frame > 0);

  // create a copy of the previous cel
  Cel* src_cel = static_cast<LayerImage*>(layer)->getCel(frame-1);
  Image* src_image = src_cel ? m_sprite->getStock()->getImage(src_cel->image):
			       NULL;

  // nothing to copy, it will be a transparent cel
  if (!src_image)
    return;

  // copy the image
  Image* dst_image = image_new_copy(src_image);
  int image_index = addImageInStock(dst_image);

  // create the new cel
  Cel* dst_cel = cel_new(frame, image_index);
  if (src_cel) {		// copy the data from the previous cel
    cel_set_position(dst_cel, src_cel->x, src_cel->y);
    cel_set_opacity(dst_cel, src_cel->opacity);
  }

  // add the cel in the layer
  addCel(static_cast<LayerImage*>(layer), dst_cel);
}

void UndoTransaction::addCel(LayerImage* layer, Cel* cel)
{
  ASSERT(layer);
  ASSERT(cel);

  if (isEnabled())
    m_undoHistory->undo_add_cel(layer, cel);

  layer->addCel(cel);
}

void UndoTransaction::removeCel(LayerImage* layer, Cel* cel)
{
  ASSERT(layer);
  ASSERT(cel);

  // find if the image that use the cel to remove, is used by
  // another cels
  bool used = false;
  for (int frame=0; frame<m_sprite->getTotalFrames(); ++frame) {
    Cel* it = layer->getCel(frame);
    if (it && it != cel && it->image == cel->image) {
      used = true;
      break;
    }
  }

  // if the image is only used by this cel,
  // we can remove the image from the stock
  if (!used)
    removeImageFromStock(cel->image);

  if (isEnabled())
    m_undoHistory->undo_remove_cel(layer, cel);

  // remove the cel from the layer
  layer->removeCel(cel);

  // and here we destroy the cel
  cel_free(cel);
}

void UndoTransaction::setCelFramePosition(Cel* cel, int frame)
{
  ASSERT(cel);
  ASSERT(frame >= 0);

  if (isEnabled())
    m_undoHistory->undo_int(cel, &cel->frame);

  cel->frame = frame;
}

void UndoTransaction::setCelPosition(Cel* cel, int x, int y)
{
  ASSERT(cel);

  if (isEnabled()) {
    m_undoHistory->undo_int(cel, &cel->x);
    m_undoHistory->undo_int(cel, &cel->y);
  }

  cel->x = x;
  cel->y = y;
}

void UndoTransaction::setFrameDuration(int frame, int msecs)
{
  if (isEnabled())
    m_undoHistory->undo_set_frlen(m_sprite, frame);

  m_sprite->setFrameDuration(frame, msecs);
}

void UndoTransaction::setConstantFrameRate(int msecs)
{
  if (isEnabled()) {
    for (int fr=0; fr<m_sprite->getTotalFrames(); ++fr)
      m_undoHistory->undo_set_frlen(m_sprite, fr);
  }

  m_sprite->setDurationForAllFrames(msecs);
}

void UndoTransaction::moveFrameBefore(int frame, int before_frame)
{
  if (frame != before_frame &&
      frame >= 0 &&
      frame < m_sprite->getTotalFrames() &&
      before_frame >= 0 &&
      before_frame < m_sprite->getTotalFrames()) {
    // change the frame-lengths...

    int frlen_aux = m_sprite->getFrameDuration(frame);

    // moving the frame to the future
    if (frame < before_frame) {
      for (int c=frame; c<before_frame-1; c++)
	setFrameDuration(c, m_sprite->getFrameDuration(c+1));

      setFrameDuration(before_frame-1, frlen_aux);
    }
    // moving the frame to the past
    else if (before_frame < frame) {
      for (int c=frame; c>before_frame; c--)
	setFrameDuration(c, m_sprite->getFrameDuration(c-1));

      setFrameDuration(before_frame, frlen_aux);
    }

    // change the cels of position...
    moveFrameBeforeLayer(m_sprite->getFolder(), frame, before_frame);
  }
}

void UndoTransaction::moveFrameBeforeLayer(Layer* layer, int frame, int before_frame)
{
  ASSERT(layer);

  switch (layer->getType()) {

    case GFXOBJ_LAYER_IMAGE: {
      CelIterator it = ((LayerImage*)layer)->getCelBegin();
      CelIterator end = ((LayerImage*)layer)->getCelEnd();

      for (; it != end; ++it) {
	Cel* cel = *it;
	int new_frame = cel->frame;

	// moving the frame to the future
	if (frame < before_frame) {
	  if (cel->frame == frame) {
	    new_frame = before_frame-1;
	  }
	  else if (cel->frame > frame &&
		   cel->frame < before_frame) {
	    new_frame--;
	  }
	}
	// moving the frame to the past
	else if (before_frame < frame) {
	  if (cel->frame == frame) {
	    new_frame = before_frame;
	  }
	  else if (cel->frame >= before_frame &&
		   cel->frame < frame) {
	    new_frame++;
	  }
	}

	if (cel->frame != new_frame)
	  setCelFramePosition(cel, new_frame);
      }
      break;
    }

    case GFXOBJ_LAYER_FOLDER: {
      LayerIterator it = static_cast<LayerFolder*>(layer)->get_layer_begin();
      LayerIterator end = static_cast<LayerFolder*>(layer)->get_layer_end();

      for (; it != end; ++it)
	moveFrameBeforeLayer(*it, frame, before_frame);
      break;
    }

  }
}

Cel* UndoTransaction::getCurrentCel()
{
  if (m_sprite->getCurrentLayer() && m_sprite->getCurrentLayer()->is_image())
    return static_cast<LayerImage*>(m_sprite->getCurrentLayer())->getCel(m_sprite->getCurrentFrame());
  else
    return NULL;
}

void UndoTransaction::cropCel(Cel* cel, int x, int y, int w, int h, int bgcolor)
{
  Image* cel_image = m_sprite->getStock()->getImage(cel->image);
  ASSERT(cel_image);
    
  // create the new image through a crop
  Image* new_image = image_crop(cel_image, x-cel->x, y-cel->y, w, h, bgcolor);

  // replace the image in the stock that is pointed by the cel
  replaceStockImage(cel->image, new_image);

  // update the cel's position
  setCelPosition(cel, x, y);
}

Image* UndoTransaction::getCelImage(Cel* cel)
{
  if (cel && cel->image >= 0 && cel->image < m_sprite->getStock()->size())
    return m_sprite->getStock()->getImage(cel->image);
  else
    return NULL;
}

// clears the mask region in the current sprite with the specified background color
void UndoTransaction::clearMask(int bgcolor)
{
  Cel* cel = getCurrentCel();
  if (!cel)
    return;

  Image* image = getCelImage(cel);
  if (!image)
    return;

  // if the mask is empty then we have to clear the entire image
  // in the cel
  if (m_sprite->getMask()->is_empty()) {
    // if the layer is the background then we clear the image
    if (m_sprite->getCurrentLayer()->is_background()) {
      if (isEnabled())
	m_undoHistory->undo_image(image, 0, 0, image->w, image->h);

      // clear all
      image_clear(image, bgcolor);
    }
    // if the layer is transparent we can remove the cel (and its
    // associated image)
    else {
      removeCel(static_cast<LayerImage*>(m_sprite->getCurrentLayer()), cel);
    }
  }
  else {
    int offset_x = m_sprite->getMask()->x-cel->x;
    int offset_y = m_sprite->getMask()->y-cel->y;
    int u, v, putx, puty;
    int x1 = MAX(0, offset_x);
    int y1 = MAX(0, offset_y);
    int x2 = MIN(image->w-1, offset_x+m_sprite->getMask()->w-1);
    int y2 = MIN(image->h-1, offset_y+m_sprite->getMask()->h-1);

    // do nothing
    if (x1 > x2 || y1 > y2)
      return;

    if (isEnabled())
      m_undoHistory->undo_image(image, x1, y1, x2-x1+1, y2-y1+1);

    // clear the masked zones
    for (v=0; v<m_sprite->getMask()->h; v++) {
      div_t d = div(0, 8);
      ase_uint8* address = ((ase_uint8 **)m_sprite->getMask()->bitmap->line)[v]+d.quot;

      for (u=0; u<m_sprite->getMask()->w; u++) {
	if ((*address & (1<<d.rem))) {
	  putx = u + offset_x;
	  puty = v + offset_y;
	  image_putpixel(image, putx, puty, bgcolor);
	}

	_image_bitmap_next_bit(d, address);
      }
    }
  }
}

void UndoTransaction::flipImage(Image* image, int x1, int y1, int x2, int y2,
			 bool flip_horizontal, bool flip_vertical)
{
  // insert the undo operation
  if (isEnabled()) {
    if (flip_horizontal)
      m_undoHistory->undo_flip(image, x1, y1, x2, y2, true);

    if (flip_vertical)
      m_undoHistory->undo_flip(image, x1, y1, x2, y2, false);
  }

  // flip the portion of the bitmap
  Image* area = image_crop(image, x1, y1, x2-x1+1, y2-y1+1, 0);
  int x, y;

  for (y=0; y<(y2-y1+1); y++)
    for (x=0; x<(x2-x1+1); x++)
      image_putpixel(image,
		     flip_horizontal ? x2-x: x1+x,
		     flip_vertical ? y2-y: y1+y,
		     image_getpixel(area, x, y));

  image_free(area);
}

void UndoTransaction::pasteImage(const Image* src_image, int x, int y, int opacity)
{
  const Layer* layer = m_sprite->getCurrentLayer();

  ASSERT(layer);
  ASSERT(layer->is_image());
  ASSERT(layer->is_readable());
  ASSERT(layer->is_writable());

  Cel* cel = ((LayerImage*)layer)->getCel(m_sprite->getCurrentFrame());
  ASSERT(cel);

  Image* cel_image = m_sprite->getStock()->getImage(cel->image);
  Image* cel_image2 = image_new_copy(cel_image);
  image_merge(cel_image2, src_image, x-cel->x, y-cel->y, opacity, BLEND_MODE_NORMAL);

  replaceStockImage(cel->image, cel_image2); // TODO fix this, improve, avoid replacing the whole image
}

void UndoTransaction::copyToCurrentMask(Mask* mask)
{
  ASSERT(m_sprite->getMask());
  ASSERT(mask);

  if (isEnabled())
    m_undoHistory->undo_set_mask(m_sprite);

  mask_copy(m_sprite->getMask(), mask);
}

void UndoTransaction::setMaskPosition(int x, int y)
{
  ASSERT(m_sprite->getMask());

  if (isEnabled()) {
    m_undoHistory->undo_int(m_sprite->getMask(), &m_sprite->getMask()->x);
    m_undoHistory->undo_int(m_sprite->getMask(), &m_sprite->getMask()->y);
  }

  m_sprite->getMask()->x = x;
  m_sprite->getMask()->y = y;
}

void UndoTransaction::deselectMask()
{
  // Destroy the *deselected* mask
  Mask* mask = m_sprite->requestMask("*deselected*");
  if (mask) {
    m_sprite->removeMask(mask);
    mask_free(mask);
  }

  // Save the selection in the repository
  mask = mask_new_copy(m_sprite->getMask());
  mask_set_name(mask, "*deselected*");
  m_sprite->addMask(mask);

  if (isEnabled())
    m_undoHistory->undo_set_mask(m_sprite);

  /// Deselect the mask
  mask_none(m_sprite->getMask());
}